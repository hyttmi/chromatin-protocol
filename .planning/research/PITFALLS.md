# Pitfalls Research

**Domain:** Client-side PQ envelope encryption for chromatindb Python SDK
**Researched:** 2026-03-31
**Confidence:** HIGH (verified against FIPS 203, IETF drafts, CMS-KEM spec, existing codebase, and NIST SP 800-227)

## Critical Pitfalls

### Pitfall 1: Treating ML-KEM as Public-Key Encryption (It Is a KEM)

**What goes wrong:**
The developer designs the envelope encryption system assuming ML-KEM can "encrypt a specific DEK to a recipient's public key" -- like RSA-OAEP would. They generate a random 32-byte DEK, then try to encapsulate it to the recipient. This is not how KEMs work. ML-KEM's `encapsulate()` generates its own random shared secret; the caller cannot choose what value gets encapsulated. The result: trying to pass a DEK into `encapsulate()` as input, getting confused by the API, or incorrectly using the shared secret as the DEK directly (breaking multi-recipient).

Concretely with liboqs-python: `kem.encap_secret(recipient_pk)` returns `(ciphertext, shared_secret)` where `shared_secret` is random and cannot be chosen. There is no way to make it equal to a pre-existing DEK.

**Why it happens:**
RSA-KEM and RSA-OAEP allow the sender to encrypt an arbitrary message (like a DEK) to a public key. ML-KEM fundamentally cannot do this -- the IETF security considerations draft explicitly states "ML-KEM is not a drop-in replacement for RSA-KEM as RSA-KEM can encapsulate the same shared secret to many recipients whereas ML-KEM cannot." Developers familiar with RSA envelope encryption carry that mental model forward.

**How to avoid:**
Use the KEM-then-Wrap pattern (matching CMS KEMRecipientInfo from RFC 9629):

```python
# CORRECT: KEM-then-Wrap pattern
# 1. Generate a random DEK
dek = os.urandom(32)

# 2. For each recipient, encapsulate to get a per-recipient shared secret
ciphertext, shared_secret = kem.encap_secret(recipient_pk)

# 3. Derive a key-encryption key (KEK) from the shared secret via HKDF
kek = hkdf_derive(salt=b"", ikm=shared_secret,
                   info=b"chromatindb-envelope-kek-v1", length=32)

# 4. Wrap the DEK with the KEK using AEAD
wrapped_dek = aead_encrypt(plaintext=dek, ad=context_bytes,
                           nonce=nonce, key=kek)

# 5. Store (kem_ciphertext, wrapped_dek) per recipient
```

```python
# WRONG: Trying to encapsulate a chosen DEK
dek = os.urandom(32)
ciphertext = kem.encapsulate(recipient_pk, dek)  # API doesn't work this way!
```

The KEM gives you a random shared secret; you derive a KEK from it; the KEK wraps your actual DEK. This two-layer approach is the only correct pattern for multi-recipient KEM envelope encryption.

**Warning signs:**
- Code that passes the DEK as an argument to `encap_secret()`
- Single encapsulate call expected to produce the same shared secret for multiple recipients
- No HKDF step between KEM output and AEAD key-wrap
- Tests that assume `encapsulate` is deterministic

**Phase to address:**
Envelope encryption implementation phase (likely Phase 1 of the milestone). The KEM-then-Wrap pattern must be established in the first plan that touches ML-KEM for encryption, before any multi-recipient work begins.

---

### Pitfall 2: Multi-Recipient Shared Secret Assumption

**What goes wrong:**
For multi-recipient encryption, the developer encapsulates to each recipient and uses each recipient's unique shared secret as the per-recipient DEK. This means the data must be encrypted N times (once per recipient), because each recipient has a different key. At 100 MiB blobs and 50 group members, this means 5 GiB of ciphertext for a single blob. Alternatively, the developer tries to make all recipients get the same shared secret, which ML-KEM cannot do.

**Why it happens:**
In RSA-based envelope encryption, you simply "encrypt the DEK to each recipient's public key." All recipients get the same DEK, and the data is encrypted once. With ML-KEM, each encapsulation produces a different random shared secret, so the naive approach gives each recipient a different key.

**How to avoid:**
One DEK, one data encryption, N key wraps:

1. Generate a single random DEK (32 bytes)
2. Encrypt the blob data once with the DEK (ChaCha20-Poly1305)
3. For each recipient:
   a. `encap_secret(recipient_pk)` -> `(ct_i, ss_i)`
   b. Derive KEK_i from ss_i via HKDF
   c. Wrap the DEK with KEK_i -> `wrapped_dek_i`
4. Store: `[encrypted_data, [(ct_1, wrapped_dek_1), (ct_2, wrapped_dek_2), ...]]`

Each recipient decapsulates their own `ct_i` to recover `ss_i`, derives `KEK_i`, unwraps the DEK, and decrypts the data. The data ciphertext is stored once. The per-recipient overhead is just ~1600 bytes (KEM ciphertext) + ~48 bytes (wrapped DEK with nonce + tag).

**Warning signs:**
- Blob data encrypted multiple times (once per recipient)
- Storage multiplier proportional to recipient count
- No distinction between DEK and KEK in the code

**Phase to address:**
Envelope encryption implementation phase. The data structure for encrypted blobs must be designed with multi-recipient in mind from day one -- retrofitting is a rewrite.

---

### Pitfall 3: Content-Addressed Hashing on Encrypted Data (Dedup Breaks)

**What goes wrong:**
chromatindb uses `SHA3-256(FlatBuffer(blob))` as the blob ID. When the same plaintext is encrypted twice (with different random nonces, different KEM shared secrets), it produces completely different ciphertext. Therefore, the same logical document written twice produces two different blob hashes and gets stored twice. The developer either:
- (a) Is surprised that dedup stopped working and tries to "fix" it
- (b) Tries convergent encryption (deriving the key from the plaintext hash) which leaks information
- (c) Doesn't notice and storage bloats silently

**Why it happens:**
The existing system was designed around content-addressing where the same data produces the same hash. Encryption fundamentally breaks this property -- same plaintext with different nonces or different KEM encapsulations produces different ciphertext. This is a security feature, not a bug (IND-CPA security requires randomized encryption), but it conflicts with the storage model's dedup assumption.

**How to avoid:**
Accept that dedup breaks for encrypted blobs. This is the correct tradeoff. Document it explicitly:

1. Encrypted blobs do not deduplicate. Each write produces a unique blob hash. This is expected and correct.
2. Do NOT use convergent encryption (deriving the encryption key from `SHA3-256(plaintext)`). This leaks whether two encrypted blobs contain the same plaintext -- a classic side-channel attack. Academic literature has extensively documented this weakness.
3. Client-side dedup (if needed in the future) should happen before encryption: the client checks if it already has a blob hash for this plaintext before encrypting and writing. But this requires client-side state.
4. The namespace's sequence numbers and timestamps are the proper way to identify "latest version" of a logical document -- not content-addressing. Application-layer indexing (e.g., a metadata blob mapping filenames to blob hashes) handles this.

The blob hash still serves its purpose: it uniquely identifies each encrypted blob, enables integrity verification, and the signature proves authorship. It just no longer serves as a dedup mechanism.

**Warning signs:**
- Code that computes a hash of the plaintext to use as a blob identifier
- Deriving encryption keys from plaintext content
- Tests asserting that writing the same data twice produces the same blob hash
- Storage growth concerns attributed to "encryption overhead"

**Phase to address:**
First encrypted write/read phase. Must be a documented design decision in the plan, not discovered during testing. The README / getting-started guide should explicitly call out "encrypted blobs do not deduplicate."

---

### Pitfall 4: Nonce Reuse Between Transport AEAD and Data AEAD

**What goes wrong:**
The SDK already uses ChaCha20-Poly1305 IETF (12-byte nonce, counter-based) for the transport layer. The developer reuses the same nonce management approach (incrementing counter) for data encryption. But transport AEAD and data AEAD have fundamentally different requirements:

- **Transport AEAD:** Sequential counter, never reused because connection is stateful and sequential. Single key per session. Counter starts at 1 post-handshake.
- **Data AEAD:** Random nonces required because the same key may encrypt many independent blobs without sequential state. Or different keys per blob (then nonce can be anything).

If the developer uses a counter-based nonce with the data encryption key and that counter resets (client restart, reconnect, different machine), the same (key, nonce) pair encrypts different data. ChaCha20-Poly1305 with nonce reuse leaks the XOR of plaintexts and enables authentication tag forgery.

**Why it happens:**
The existing `_framing.py` transport layer uses counter-based nonces successfully. Copy-pasting that pattern for data encryption seems natural. But transport nonces work because (a) there is exactly one counter per key per direction, and (b) the key is ephemeral (session-scoped). Data encryption keys may persist across sessions.

**How to avoid:**
Use random nonces for data AEAD, matching the existing C++ DARE pattern in `storage.cpp`:

```python
# CORRECT: Random nonce for data encryption (matches C++ DARE pattern)
nonce = os.urandom(12)  # 12 bytes, random
ct = aead_encrypt(plaintext=data, ad=ad, nonce=nonce, key=dek)
envelope = version_byte + nonce + ct  # [1][12][N+16]
```

With IETF ChaCha20-Poly1305 (12-byte nonce), the birthday bound collision probability at 2^32 messages under one key gives a 2^-32 collision chance. This is acceptable for per-blob DEKs (each key encrypts exactly one blob). If a single DEK ever encrypted more than 2^32 messages, you would need XChaCha20-Poly1305 (24-byte nonce) instead -- but the envelope design uses a fresh DEK per blob, so this is not a concern.

Critically: the data AEAD key must be completely independent from the transport session key. Different HKDF context labels enforce this separation.

**Warning signs:**
- `send_counter` or any incrementing counter used for data encryption nonces
- Data encryption using the session key from the handshake
- No `os.urandom()` call in the data encryption path
- Same HKDF context label for transport keys and data encryption keys

**Phase to address:**
Envelope encryption implementation phase. The nonce generation strategy must be specified in the plan and verified in unit tests (test that two encryptions of the same plaintext produce different ciphertext).

---

### Pitfall 5: Encrypted Blob Format Without Version Byte

**What goes wrong:**
The developer designs an encrypted blob format without a version byte (or with an internal version that cannot be read without decryption). When the encryption scheme needs to change (algorithm migration, format extension, new KEM), every existing encrypted blob becomes unreadable because there is no way to determine which decryption path to use without trying them all. Alternatively, the developer puts the version inside the encrypted payload, requiring decryption before version detection -- creating a chicken-and-egg problem.

**Why it happens:**
The current system already has a version byte for DARE (`ENCRYPTION_VERSION` in `storage.cpp`), but that is a node-internal concern. The client-side envelope format is a new format that lives inside the blob's `data` field (which the node treats as opaque bytes). Developers may not think to add versioning because "we only have one version" -- but PQ crypto is an evolving field and the format will change.

**How to avoid:**
Define a clear envelope format with a plaintext header:

```
Encrypted Blob Data Field:
[1 byte: envelope version (0x01)]
[2 bytes: recipient count (BE uint16)]
For each recipient:
    [1568 bytes: ML-KEM-1024 ciphertext]
    [12 bytes: wrap nonce]
    [48 bytes: wrapped DEK (32 + 16 tag)]
[12 bytes: data nonce]
[N + 16 bytes: data ciphertext + tag]
```

Key design rules:
1. **Version byte is first and plaintext.** Anyone can read it without decryption.
2. **Recipient count is plaintext.** Allows parsing the per-recipient blocks without decryption.
3. **KEM ciphertext is per-recipient.** Each recipient has their own block.
4. **Data ciphertext is shared.** Encrypted once with the DEK.

Reserve version 0x00 as invalid (catches uninitialized memory). Start at 0x01. When ML-KEM-1024 is replaced (or the AEAD changes), bump to 0x02 and the new code tries v2 first, falls back to v1.

**Warning signs:**
- No version byte in the encrypted format
- Version byte inside the encrypted payload
- No plan for algorithm migration
- Tests that hardcode magic offsets without referencing a version

**Phase to address:**
Envelope encryption format design (first phase). This must be locked before any encrypted blobs are written to storage, because changing the format after data exists requires migration.

---

### Pitfall 6: Key Directory Trust Without Consistency Guarantees

**What goes wrong:**
The pubkey directory (org directory namespace) stores user public keys as signed blobs. Client A writes a new key for User X. Client B reads User X's key but gets a stale version (not yet replicated). Client B encrypts a message to User X's old key. If the old key was rotated due to compromise, Client B just encrypted data to a compromised key. Alternatively: an admin publishes a revocation but the recipient's SDK cache still has the old key.

The directory is eventually consistent (blob replication is not instant). Any encryption decision based on directory state is based on a potentially stale snapshot.

**Why it happens:**
chromatindb is a decentralized replicated system with eventual consistency. There is no consensus protocol, no linearizable reads, no way to guarantee "I have the latest directory." The developer designs the directory as if reads are strongly consistent ("I fetched the key, so it is current"), but replication lag means the directory state can be arbitrarily behind.

**How to avoid:**
Accept eventual consistency and design around it:

1. **Key lookup returns a timestamp.** The SDK shows when the key was published. The application decides if it is "fresh enough."
2. **Encryption to stale keys is not a security failure -- it is a liveness failure.** The recipient simply cannot decrypt (they no longer have the old private key after rotation). The sender retries with the updated key. This is annoying but not dangerous.
3. **Revocation is best-effort, not instant.** Document that revoking a key does not retroactively protect data already encrypted to that key. Revocation prevents *future* encryption to the compromised key once the tombstone propagates.
4. **Cache TTL on directory entries.** The SDK should re-fetch directory entries periodically (e.g., every 60 seconds) rather than caching indefinitely. The `TimeRangeRequest` query can efficiently check for updates.
5. **Do not encrypt to keys older than a threshold without warning.** If a fetched key's timestamp is more than N hours old, the SDK should log a warning or raise. This catches the "completely disconnected from directory" case.
6. **Pin-on-first-use (TOFU) is acceptable for initial trust.** The first time you see a user's key, trust it. If it changes, alert. This matches SSH's model and is appropriate for a decentralized system without a CA.

**Warning signs:**
- Code that fetches a key once and caches it forever
- No timestamp checking on directory entries
- Revocation logic that assumes "tombstone published = everyone stopped using old key"
- No retry logic for "recipient says they can't decrypt"

**Phase to address:**
Directory/user discovery phase. The cache invalidation and freshness strategy must be designed before the encryption helpers use directory keys. Group resolution (Pitfall 8) also depends on this.

---

### Pitfall 7: ML-KEM Encapsulation Key Stored in Directory Without Validation

**What goes wrong:**
Each user publishes an ML-KEM-1024 encapsulation (public) key to the directory. When another user fetches this key to encrypt data, they do not validate it. A corrupted or tampered key (bit flip in transit, malicious edit, truncated blob) causes `encap_secret()` to either: (a) produce a ciphertext that cannot be decapsulated (silent data loss), (b) crash the KEM implementation, or (c) leak information about the encapsulation process.

The IETF ML-KEM security considerations draft explicitly warns: "a public key can be 'poisoned' such that a future adversary can recover the private key even though it will appear correct in normal usage." Additionally, FIPS 203 requires that both parties perform key checks: the Encapsulation Key Check on the public key, and the Decapsulation Key Check on the ciphertext.

**Why it happens:**
The existing signing key (`Identity.from_public_key()`) validates key size but not key structure. ML-DSA-87 public keys have a fixed 2592-byte size, and invalid keys simply produce verification failures. ML-KEM-1024 encapsulation keys also have a fixed size (1568 bytes), but FIPS 203 adds a modular reduction check that ensures the key encodes valid polynomial coefficients. Developers assume "right size = valid key" by analogy with the signing key validation.

**How to avoid:**
Validate encapsulation keys before use:

1. **Size check:** ML-KEM-1024 encapsulation key must be exactly 1568 bytes.
2. **Modular arithmetic check:** FIPS 203 Section 7.2 specifies an Encapsulation Key Check where the key is decoded and re-encoded to verify that all polynomial coefficients are in the valid range. liboqs may perform this internally during `encap_secret()` -- verify this and document it. If liboqs does not validate, implement the check.
3. **Sign the encapsulation key.** The user's ML-DSA-87 identity should sign their ML-KEM-1024 encapsulation key when publishing it. Verifiers check the signature before trusting the encapsulation key. This prevents tampering in transit and binds the encryption key to the signing identity.
4. **Test with malformed keys.** Unit tests should include truncated keys, all-zero keys, and keys with out-of-range coefficients.

**Warning signs:**
- `encap_secret()` called on a key with only a length check
- No signature binding between signing identity and encryption key
- No tests for malformed encapsulation keys
- Directory entries that store raw encapsulation keys without a signature

**Phase to address:**
Self-registration / directory phase (when users publish keys). The validation must be in place before any encryption uses directory-sourced keys.

---

### Pitfall 8: Group Membership Race Conditions

**What goes wrong:**
Groups are stored as named member sets in the directory namespace. When encrypting to a group, the SDK resolves the group membership list, fetches each member's encapsulation key, and encrypts. But between the group resolution and the encryption:
- A member was removed from the group (they should not receive the message, but already got a wrapped DEK)
- A member was added to the group (they should receive the message, but their key was not fetched)
- A member rotated their key (the SDK encrypted to the old key)

In a decentralized system with eventual consistency, these races are not edge cases -- they are the normal operating condition.

**Why it happens:**
The group membership list and the member keys are separate blobs in the directory namespace. Fetching them is not atomic. Replication lag means different clients see different group states at the same time. There is no locking or transaction mechanism.

**How to avoid:**
Design for the race, not against it:

1. **Snapshot semantics.** When encrypting to a group, fetch the group membership blob and all member keys. Record the group blob's hash and the member key hashes in the encrypted envelope metadata. This is "encrypted at group state X" -- not "encrypted to the current group."
2. **Accept over-encryption.** If a removed member received a wrapped DEK, they can decrypt that specific message. This is acceptable because:
   - Revocation is about preventing *future* access, not retroactive denial
   - Re-encryption on every membership change is prohibitively expensive
   - The removed member could have read the data before removal anyway
3. **Accept under-encryption.** If a new member was not included, they cannot decrypt that specific message. The sender can re-encrypt if needed. This is a liveness issue, not a security issue.
4. **Group versioning.** Each group membership change increments a version (the blob's seq_num in the directory). The encrypted envelope records which group version it was encrypted for. Recipients who cannot decrypt can request re-encryption referencing the group version.
5. **Do not re-encrypt existing data on group changes.** This is an anti-feature that scales with (data_size * change_frequency). Instead, new data uses new group state. Old data remains readable by whoever was in the group at write time.

**Warning signs:**
- Locks or transactions in the group resolution code (impossible in a decentralized system)
- Re-encryption triggered by group membership changes
- Tests that assume atomic group + key fetch
- No group version tracking in encrypted envelopes

**Phase to address:**
Groups phase. Group resolution logic must document these race semantics. Integration tests should verify that encryption works with stale group state (no crash, no silent failure).

---

### Pitfall 9: Missing HKDF Context Separation Between Key Derivation Domains

**What goes wrong:**
The system already uses HKDF-SHA256 in three contexts: transport key derivation (from ML-KEM shared secret), DARE key derivation (from master key), and now envelope encryption key derivation (from ML-KEM shared secret for wrapping). If any two of these use the same HKDF info/context label, keys derived in different contexts could collide if the IKM happens to be the same, breaking domain separation.

Specifically: the transport handshake uses `"chromatin-init-to-resp-v1"` as the info label with the KEM shared secret as IKM. If the envelope encryption KEK derivation also uses a similar label, and if the IKM (different KEM shared secret) happened to collide (2^-256 probability but defense-in-depth matters), the derived keys would be identical.

**Why it happens:**
Developers see "we already have HKDF, just call it" and do not think about domain separation. Each HKDF context label creates a separate key derivation domain. Reusing labels across domains (transport vs envelope vs DARE) is a subtle but real cryptographic error.

**How to avoid:**
Establish a naming convention for HKDF context labels and document all existing labels:

| Domain | HKDF Info Label | IKM Source |
|--------|----------------|------------|
| Transport (init->resp) | `chromatin-init-to-resp-v1` | KEM session shared secret |
| Transport (resp->init) | `chromatin-resp-to-init-v1` | KEM session shared secret |
| DARE | `chromatindb-dare-v1` | Node master key |
| Envelope KEK | `chromatindb-envelope-kek-v1` | Per-recipient KEM shared secret |

Rules:
- Every new HKDF usage gets a unique context label
- Labels include the version (`-v1`) for future migration
- Labels include the domain (`dare`, `envelope`, `transport`)
- All labels are documented in PROTOCOL.md alongside their existing counterparts

**Warning signs:**
- HKDF calls without a context/info parameter
- Reusing an existing context label for a new purpose
- No central registry of HKDF labels in the codebase or docs

**Phase to address:**
Envelope encryption implementation phase. The label must be chosen in the plan and added to PROTOCOL.md. This is a one-line decision but getting it wrong is a silent cryptographic failure.

---

### Pitfall 10: ML-KEM Keypair Lifecycle Confusion (Signing vs Encryption Identity)

**What goes wrong:**
Each user currently has one identity: an ML-DSA-87 signing keypair. The namespace is `SHA3-256(signing_pubkey)`. For envelope encryption, each user needs an additional ML-KEM-1024 keypair (encapsulation key for others to encrypt to, decapsulation key to decrypt). The developer either:
- (a) Tries to use the ML-DSA-87 signing key for ML-KEM operations (incompatible algorithms)
- (b) Creates a separate ML-KEM identity with no cryptographic binding to the signing identity
- (c) Generates the ML-KEM keypair ephemerally instead of persisting it (every restart generates new keys, breaking decryption of old data)
- (d) Stores the ML-KEM decapsulation key in the directory (catastrophic -- private keys must never be published)

**Why it happens:**
The existing `Identity` class manages only ML-DSA-87 keys. Adding a second keypair requires either extending `Identity` or creating a parallel class. The relationship between the two keypairs (the signing key authenticates the encryption key) is not obvious from the code structure. And ML-DSA-87 / ML-KEM-1024 look similar ("they are both liboqs") but serve completely different purposes.

**How to avoid:**
Extend the identity model cleanly:

1. **One identity, two keypairs.** A user has:
   - ML-DSA-87 signing keypair (existing, defines namespace)
   - ML-KEM-1024 encryption keypair (new, published to directory)
2. **Signing key authenticates encryption key.** The user publishes their ML-KEM-1024 encapsulation key as a signed blob in the directory, signed by their ML-DSA-87 key. Anyone fetching the encryption key verifies the signature.
3. **Encryption keypair persisted alongside signing keypair.** The `.key` / `.pub` file convention extends: add `.enc.key` (ML-KEM-1024 decapsulation key, 3168 bytes) and `.enc.pub` (ML-KEM-1024 encapsulation key, 1568 bytes).
4. **Decapsulation key NEVER published.** The directory stores only the encapsulation (public) key. The decapsulation (private) key stays on the user's machine.
5. **Key rotation = new keypair + new directory blob.** The old keypair must be retained to decrypt data encrypted to the old key. A key history (or key chain) pattern is needed.

**Warning signs:**
- ML-KEM keypair generated in memory but not saved to disk
- No file convention for encryption keys
- Decapsulation key passed to any "publish" or "write to directory" function
- No signature binding between signing and encryption keys
- Old decapsulation keys discarded after rotation

**Phase to address:**
Self-registration phase (when users generate and publish keys). The identity model extension must happen before any envelope encryption code, because encryption depends on having persistable, authenticated encryption keys.

---

## Technical Debt Patterns

| Shortcut | Immediate Benefit | Long-term Cost | When Acceptable |
|----------|-------------------|----------------|-----------------|
| Hardcode single recipient (no multi-recipient) | Simpler format, faster shipping | Rewrite encrypted blob format for groups | Never -- design multi-recipient from day one, even if groups ship later |
| Cache directory keys forever | No re-fetch overhead | Encrypt to revoked/rotated keys indefinitely | Never -- even a long TTL (5 min) is better than forever |
| Skip encapsulation key validation | Fewer dependencies on FIPS 203 internals | Silent data loss on corrupted keys, potential key recovery attack | Never -- validate or at minimum sign the encapsulation key |
| One HKDF label for all envelope operations | Less ceremony | Domain separation violation if scheme evolves | Never -- labels are cheap, collisions are catastrophic |
| No envelope version byte | Simpler parser | Cannot migrate to new algorithms without breaking old data | Never -- one byte prevents a rewrite |
| Convergent encryption for dedup | Restores content-addressing dedup | Leaks plaintext equality (IND-CPA violation), well-known attack | Never -- accept that encrypted blobs do not dedup |

## Integration Gotchas

| Integration | Common Mistake | Correct Approach |
|-------------|----------------|------------------|
| liboqs `encap_secret()` | Passing a chosen key as an argument | Treat return value as the key; wrap your DEK with it |
| liboqs ML-KEM keypair | Using `oqs.Signature` API patterns for `oqs.KeyEncapsulation` | `kem = oqs.KeyEncapsulation("ML-KEM-1024"); pk = kem.generate_keypair()` returns encapsulation key; `kem.decap_secret(ct)` recovers shared secret |
| Existing `Identity` class | Extending it to hold KEM keys in the same oqs.Signature object | Create a separate `oqs.KeyEncapsulation` instance; ML-DSA-87 and ML-KEM-1024 are completely different algorithm types in liboqs |
| Existing `crypto.aead_encrypt()` | Reusing transport nonce counter pattern for data AEAD | Use `os.urandom(12)` for data nonces; transport counter pattern is only for session-scoped sequential messages |
| Existing `build_signing_input()` | Signing the encrypted data instead of signing metadata | Sign a canonical representation that includes the plaintext hash or the DEK commitment, not the ciphertext (which changes every encryption) |
| Existing `client.write()` | Wrapping `client.write()` and assuming WriteAck blob_hash is deterministic | Encrypted blobs produce different hashes every time; use server-returned blob_hash exclusively |
| Node blob validation | Assuming the node validates encrypted content structure | The node validates namespace ownership, signature, size. It has zero knowledge of the envelope format inside the `data` field. All envelope format validation is client-side only |

## Performance Traps

| Trap | Symptoms | Prevention | When It Breaks |
|------|----------|------------|----------------|
| ML-KEM encapsulation per recipient per write | Write latency scales linearly with group size | Batch encapsulations; profile KEM overhead (ML-KEM-1024 encap is ~0.1ms so 100 recipients = 10ms, acceptable) | >1000 recipients per write |
| Re-encrypting all data on group membership change | CPU and IO explosion on every member add/remove | Never re-encrypt; new writes use new group state, old data stays | Any group change with >10 blobs |
| Per-blob HKDF derivation for DEK | HKDF overhead on every write and read | Minimal overhead (~microseconds); do not optimize by caching derived keys (security risk) | Not a real concern -- HKDF is fast |
| Large encrypted blob envelopes | Per-recipient overhead ~1628 bytes * N recipients in blob data field | For large groups, this metadata is negligible vs blob data. For small blobs with many recipients, the metadata dominates | 100-byte blob encrypted to 100 recipients = 162,800 bytes overhead |
| Directory key fetch on every encrypt | Network round-trip per encryption operation | Cache directory keys with TTL (60s); batch-fetch group member keys | High-frequency encrypted writes |

## Security Mistakes

| Mistake | Risk | Prevention |
|---------|------|------------|
| Using convergent encryption (key = hash of plaintext) | Confirms plaintext equality across encrypted blobs; classic chosen-plaintext attack on deterministic encryption | Generate fresh random DEK per blob; accept dedup loss |
| Reusing data AEAD nonce with same key | XOR of plaintexts leaked; Poly1305 authentication broken; full plaintext recovery possible with known plaintext | Random nonce per encryption; fresh DEK per blob makes nonce reuse probability negligible |
| Publishing ML-KEM decapsulation key to directory | Anyone can decrypt all data encrypted to that key -- total confidentiality loss | Only publish encapsulation (public) key; validate key sizes at publish time |
| No signature on directory-published encapsulation key | Man-in-the-middle substitutes their own encapsulation key; all future encrypted data readable by attacker | Sign encapsulation key with ML-DSA-87 identity; verify signature before using key for encryption |
| Encrypting to a key without checking if it has been tombstoned/revoked | Data encrypted to a compromised key | Check for key revocation (tombstone) before encrypting; warn if key has no recent heartbeat/refresh |
| Same HKDF context label for envelope KEK and transport session key | Theoretical domain separation violation; could enable cross-protocol attacks if IKM collision occurs | Unique HKDF context labels per domain; document label registry |
| Storing unwrapped DEK alongside encrypted data | Defeats the purpose of encryption; anyone with blob access has the key | DEK only exists in memory during encrypt/decrypt; only wrapped DEKs stored |
| Missing associated data in key-wrap AEAD | Wrapped DEK can be transplanted between different encrypted blobs (ciphertext swapping attack) | Include blob context (namespace, timestamp, purpose) as AD in the key-wrap AEAD |

## "Looks Done But Isn't" Checklist

- [ ] **Envelope encryption:** Missing multi-recipient support -- verify N recipients can independently decrypt
- [ ] **Envelope encryption:** Missing version byte in format -- verify format starts with version, is parseable without decryption
- [ ] **Key directory:** Missing signature on encapsulation key -- verify directory entries bind signing identity to encryption key
- [ ] **Key directory:** Missing cache invalidation -- verify SDK re-fetches keys periodically, not just once
- [ ] **Key rotation:** Missing old key retention -- verify user can still decrypt data encrypted to previous keys after rotation
- [ ] **Group encryption:** Missing group version in envelope -- verify envelope records which group state it was encrypted for
- [ ] **Group encryption:** Missing stale-group handling -- verify encryption works (doesn't crash) with outdated group membership
- [ ] **Nonce management:** Missing randomness in data AEAD -- verify nonces are `os.urandom(12)` not counter-based
- [ ] **HKDF labels:** Verify all HKDF context labels are unique across transport, DARE, and envelope domains
- [ ] **Identity model:** Missing encryption keypair persistence -- verify `.enc.key`/`.enc.pub` files survive client restart
- [ ] **WriteAck handling:** Verify encrypted write tests use server-returned blob_hash, not client-computed hash
- [ ] **Decapsulation key safety:** Verify decapsulation key never appears in any published blob or directory entry

## Recovery Strategies

| Pitfall | Recovery Cost | Recovery Steps |
|---------|---------------|----------------|
| KEM misuse (encrypting data directly with KEM) | HIGH | Redesign envelope format; re-encrypt all affected blobs with correct KEM-then-Wrap pattern |
| No version byte in format | HIGH | Define v2 format with version byte; write migration tool to re-encrypt all v1 blobs; must retain v1 parser forever |
| Nonce reuse in data AEAD | CRITICAL | Identify affected blobs; re-encrypt with fresh nonces; assess what plaintext may have leaked via XOR analysis |
| Published decapsulation key | CRITICAL | Generate new keypair immediately; re-encrypt all data to new key; old key must be considered fully compromised |
| Stale directory key cache | LOW | Flush cache; re-encrypt failed messages; add TTL to cache |
| Missing signature on encapsulation key | MEDIUM | Publish signed key blob; SDK update to verify signatures; old unsigned keys should be distrusted with warning |
| Group race (wrong members received DEK) | LOW | Accept as eventual consistency artifact; re-encrypt specific blobs if security-critical |
| Missing HKDF domain separation | MEDIUM | Assign new unique labels; re-derive all keys; re-encrypt data if label collision was exploitable |

## Pitfall-to-Phase Mapping

| Pitfall | Prevention Phase | Verification |
|---------|------------------|--------------|
| ML-KEM is a KEM not encryption (Pitfall 1) | Envelope encryption implementation | Unit test: encapsulate returns random shared secret; DEK is wrapped, not encapsulated |
| Multi-recipient shared secret (Pitfall 2) | Envelope encryption format design | Unit test: 3 recipients decrypt same blob independently; data encrypted once |
| Content-addressing + encryption (Pitfall 3) | Encrypted write helpers | Test: same plaintext written twice produces different blob_hash; documented in README |
| Transport vs data AEAD nonce (Pitfall 4) | Envelope encryption implementation | Test: two encryptions of same data produce different ciphertext; no counter in data path |
| Missing version byte (Pitfall 5) | Envelope format design (first plan) | Test: encrypted blob parses version without decryption; format documented in PROTOCOL.md |
| Directory trust / staleness (Pitfall 6) | User discovery implementation | Test: SDK re-fetches keys after TTL; stale key produces warning, not crash |
| Encapsulation key validation (Pitfall 7) | Self-registration / key publishing | Test: truncated key rejected; invalid key rejected; signature verified before use |
| Group membership races (Pitfall 8) | Groups implementation | Test: encryption with stale group state succeeds; envelope contains group version |
| HKDF domain separation (Pitfall 9) | Envelope encryption implementation | Code review: all HKDF labels unique; label registry in PROTOCOL.md |
| Identity keypair lifecycle (Pitfall 10) | Self-registration / identity extension | Test: encryption keypair persisted to disk; old keys retained after rotation; decapsulation key never published |

## Sources

- [FIPS 203 -- Module-Lattice-Based Key-Encapsulation Mechanism Standard](https://csrc.nist.gov/pubs/fips/203/final)
- [NIST SP 800-227 -- Recommendations for Key-Encapsulation Mechanisms](https://nvlpubs.nist.gov/nistpubs/SpecialPublications/NIST.SP.800-227.pdf)
- [IETF draft-sfluhrer-cfrg-ml-kem-security-considerations-01](https://www.ietf.org/archive/id/draft-sfluhrer-cfrg-ml-kem-security-considerations-01.html) -- "ML-KEM is not a drop-in replacement for RSA-KEM"
- [IETF draft-ietf-lamps-cms-kyber-10](https://www.ietf.org/archive/id/draft-ietf-lamps-cms-kyber-10.html) -- CMS KEMRecipientInfo with HKDF + AES-Wrap pattern
- [ML-KEM Mythbusting -- Key Material](https://keymaterial.net/2025/11/27/ml-kem-mythbusting/) -- Static key reuse safety, implementation complexity
- [When a KEM is not enough -- Neil Madden](https://neilmadden.blog/2021/02/16/when-a-kem-is-not-enough/) -- Multi-recipient KEM-DEM, key wrapping, Tag-KEMs
- [Hybrid encryption and the KEM/DEM paradigm -- Neil Madden](https://neilmadden.blog/2021/01/22/hybrid-encryption-and-the-kem-dem-paradigm/) -- KEM-DEM architecture
- [PKE vs KEM -- Prof Bill Buchanan](https://billatnapier.medium.com/how-public-key-encryption-pke-differs-from-key-encapsulation-methods-kems-7c52ea01f87b) -- Fundamental difference: KEM generates secret, sender does not choose it
- [Cloudflare: Deep dive into post-quantum key encapsulation](https://blog.cloudflare.com/post-quantum-key-encapsulation/) -- KEM architecture
- [XChaCha20-Poly1305 -- libsodium docs](https://libsodium.gitbook.io/doc/secret-key_cryptography/aead/chacha20-poly1305/xchacha20-poly1305_construction) -- Random nonce safety with extended nonces
- [IETF ChaCha20-Poly1305 -- libsodium docs](https://libsodium.gitbook.io/doc/secret-key_cryptography/aead/chacha20-poly1305/ietf_chacha20-poly1305_construction) -- 12-byte nonce counter vs random guidance
- [Azure Blob Storage client-side encryption v1 to v2 migration](https://learn.microsoft.com/en-us/azure/storage/blobs/client-side-encryption) -- Real-world format versioning migration pain
- [Secure Deduplication of Encrypted Data (ePrint 2017/1089)](https://eprint.iacr.org/2017/1089.pdf) -- Why convergent encryption is insecure
- Existing chromatindb source: `db/storage/storage.cpp` (DARE envelope format), `db/crypto/master_key.h` (HKDF label registry), `sdk/python/chromatindb/_handshake.py` (KEM session usage), `sdk/python/chromatindb/crypto.py` (AEAD primitives)

---
*Pitfalls research for: client-side PQ envelope encryption in chromatindb Python SDK*
*Researched: 2026-03-31*
