# Architecture: Client-Side PQ Envelope Encryption (v1.7.0)

**Domain:** PQ envelope encryption, pubkey directory, groups -- integrating with existing Python SDK
**Researched:** 2026-03-31
**Confidence:** HIGH (existing SDK source fully analyzed, crypto primitives verified against liboqs/PyNaCl docs)

## Existing SDK Architecture (Starting Point)

```
sdk/python/chromatindb/
  __init__.py          Public re-exports
  client.py            ChromatinClient — async context manager, 15+ methods
  types.py             18 frozen dataclasses (WriteResult, ReadResult, etc.)
  _codec.py            31 encode/decode functions for wire payloads
  _transport.py        Background reader, request_id dispatch, send_lock
  _handshake.py        ML-KEM-1024 + ML-DSA-87 mutual auth (4-step)
  _framing.py          AEAD frame IO with nonce counter management
  crypto.py            SHA3-256, ChaCha20-Poly1305, HKDF-SHA256, build_signing_input
  identity.py          Identity class — ML-DSA-87 keypair, sign/verify, namespace derivation
  wire.py              FlatBuffers TransportMessage encode/decode
  _hkdf.py             Pure-Python HKDF-SHA256 (RFC 5869)
  exceptions.py        Hierarchy: ChromatinError > CryptoError > DecryptionError, etc.
  generated/           FlatBuffers codegen (blob_generated.py, transport_generated.py)
```

Key constraints from existing code:
- `Identity` holds ML-DSA-87 only (signing). No KEM keypair.
- `crypto.py` has ChaCha20-Poly1305 AEAD + HKDF-SHA256 + SHA3-256 -- all needed for envelope encryption.
- `client.py` write_blob() builds: signing_input -> sign -> encode_blob_payload -> send. Returns server-assigned blob_hash.
- Blobs are signed FlatBuffers: namespace + pubkey + data + ttl + timestamp + signature.
- The node is "intentionally dumb" -- it stores opaque signed blobs. Encryption is purely client-side.
- Directory/group data is stored as regular blobs with application-level magic prefixes (same pattern as delegations: 0xDE1E6A7E + pubkey).

## New Module Layout

```
sdk/python/chromatindb/
  [existing modules unchanged]

  NEW MODULES:
  _envelope.py         Envelope encrypt/decrypt (KEM encap + AEAD)
  _directory.py        Directory namespace management (user/group CRUD)
  _directory_types.py  Directory-specific dataclasses (UserEntry, Group, etc.)
```

### Why These Three Modules

**_envelope.py** -- Pure crypto, no network IO. Testable in isolation. Contains:
- `envelope_encrypt(plaintext, recipient_kem_pks) -> EncryptedBlob` (multi-recipient)
- `envelope_decrypt(encrypted_blob, kem_secret_key) -> bytes`
- Encrypted blob binary format encode/decode
- Per-blob random symmetric key generation
- ML-KEM-1024 key wrapping per recipient

**_directory.py** -- Directory operations built on existing client methods. Contains:
- `DirectoryManager` class wrapping a `ChromatinClient`
- User registration (publish pubkey to directory namespace)
- User discovery (list/fetch pubkeys)
- Group CRUD (create/update/list/resolve members)
- Delegation management for the directory namespace
- High-level `write_encrypted()` / `read_encrypted()` that combine directory lookups with envelope crypto

**_directory_types.py** -- Frozen dataclasses for directory entries. Contains:
- `UserEntry` (label, signing pubkey, KEM pubkey)
- `Group` (name, member labels)
- `EncryptedBlob` (ciphertext, wrapped keys, metadata)
- `DirectoryConfig` (admin identity, directory namespace)

### What Gets Modified

**identity.py** -- Add optional ML-KEM-1024 keypair alongside ML-DSA-87:
- `Identity.generate()` produces both signing + KEM keypairs
- `Identity.load()` / `save()` reads/writes `.kem` sibling file (backward compatible: missing .kem = signing-only)
- New properties: `kem_public_key`, `kem_secret_key`, `has_kem`
- `Identity.from_public_keys(signing_pk, kem_pk)` for verify-only + decrypt-capable identities

**client.py** -- Add convenience methods:
- `write_encrypted(data, ttl, recipients)` -- encrypt then write
- `read_encrypted(namespace, blob_hash)` -- read then decrypt
- These are thin wrappers composing `_envelope` + existing `write_blob`/`read_blob`

**types.py** -- Add new result types:
- `EncryptedWriteResult` (extends WriteResult with encryption metadata)

**exceptions.py** -- Add:
- `DirectoryError(ChromatinError)` for directory operations
- `EnvelopeError(CryptoError)` for envelope encrypt/decrypt failures

**__init__.py** -- Re-export new public types.

## Encrypted Blob Binary Format

This is the critical design decision. The encrypted blob replaces the `data` field in the existing blob structure. The node sees opaque bytes -- it never knows the content is encrypted.

### Format: `data` Field of an Encrypted Blob

```
+--------+-------+------------------------------------------+
| Offset | Size  | Field                                    |
+--------+-------+------------------------------------------+
| 0      | 4     | Magic: 0xE0 0xCR 0x1P 0x70 ("encrypt0") |
| 4      | 1     | Version: 0x01                            |
| 5      | 1     | Algorithm suite: 0x01 (ML-KEM-1024 +     |
|        |       |   ChaCha20-Poly1305 + HKDF-SHA256)       |
| 6      | 2     | Recipient count (big-endian uint16)       |
| 8      | 12    | Nonce (random, for data AEAD)             |
| 20     | var   | Wrapped keys section (per recipient):     |
|        |       |   [kem_pk_hash:32][kem_ciphertext:1568]   |
|        |       |   = 1600 bytes per recipient              |
| 20+N*  | var   | AEAD ciphertext (ChaCha20-Poly1305):      |
| 1600   |       |   encrypted plaintext + 16-byte tag       |
+--------+-------+------------------------------------------+
```

### Design Rationale

**Magic bytes (4 bytes):** `0xE0CR1P70` -- distinct from TOMBSTONE_MAGIC (0xDEADBEEF) and DELEGATION_MAGIC (0xDE1E6A7E). Allows any reader to identify encrypted blobs without out-of-band metadata.

**Version byte:** Forward compatibility. Version 0x01 = current scheme. Future versions can change algorithm suite or format.

**Algorithm suite byte:** Identifies the exact crypto algorithms. Suite 0x01 = ML-KEM-1024 (NIST Level 5) + ChaCha20-Poly1305 + HKDF-SHA256. Allows future upgrades (e.g., suite 0x02 for HQC when standardized).

**Recipient count (uint16 BE):** Max 65535 recipients per blob. Realistic limit is ~60 (100 MiB blob size / 1600 bytes per recipient key wrap leaves ample room for data).

**Wrapped keys section:** Each recipient gets:
- `kem_pk_hash` (32 bytes): SHA3-256 of recipient's ML-KEM-1024 public key. Reader scans for their own hash to find their wrapped key.
- `kem_ciphertext` (1568 bytes): ML-KEM-1024 encapsulation of the per-blob data key, using the recipient's KEM public key.

**AEAD ciphertext:** The actual encrypted data. Key is derived from the per-blob shared secret (from KEM encapsulation). Nonce is the random 12 bytes from the header. Associated data = everything before the ciphertext (magic + version + suite + count + nonce + all wrapped keys) -- binds ciphertext to its metadata.

### Why Not Per-Namespace Keys

The product direction doc (project_product_direction.md) suggests per-namespace symmetric keys with HKDF(master, salt=blob_hash). This was the initial brainstorm but has problems:

1. **Key distribution complexity:** Requires a separate key distribution namespace (`_keys`) and versioning.
2. **Revocation is a nightmare:** Revoking access means re-encrypting every blob in the namespace with a new key.
3. **Per-blob KEM is simpler and stronger:** Each blob has independent keying. Revoking a user = just stop including them as a recipient. Old blobs they could read remain readable (they had access at the time), but new blobs exclude them.
4. **No key management infrastructure:** No need for key versioning, key epochs, or re-encryption jobs.

Per-blob envelope encryption with recipient list is the standard KEM envelope pattern (HPKE-style). Use it.

## Data Flow: Encrypted Write

```
Application code:
  await client.write_encrypted(data=b"secret", ttl=3600, recipients=["alice", "bob"])

1. Resolve recipients:
   _directory.py resolves "alice" and "bob" to their ML-KEM-1024 public keys
   by reading user entries from the directory namespace.

2. Envelope encrypt (_envelope.py):
   a. Generate random 32-byte per-blob data key (os.urandom)
   b. Generate random 12-byte nonce (os.urandom)
   c. For each recipient KEM public key:
      - kem = oqs.KeyEncapsulation("ML-KEM-1024")
      - kem_ct, shared_secret = kem.encap_secret(recipient_kem_pk)
      - wrapped_key = aead_encrypt(data_key, b"", nonce_for_wrap, shared_secret)
      Correction: simpler approach -- the KEM shared secret IS the wrapped key.
      The data key is derived: data_key = HKDF(shared_secret, info="chromatin-envelope-v1", 32)
      Wait, that means each recipient gets a DIFFERENT data key. Wrong.

   CORRECT approach:
   a. Generate random 32-byte data_key (os.urandom)
   b. Generate random 12-byte nonce (os.urandom)
   c. Encrypt plaintext: ciphertext = aead_encrypt(data, AD, nonce, data_key)
   d. For each recipient KEM public key:
      - kem_ct, kem_ss = encap_secret(recipient_kem_pk)
      - wrapped_data_key = aead_encrypt(data_key, b"", wrap_nonce, kem_ss)
        where wrap_nonce = SHA3-256(kem_ct)[:12]  (deterministic from ciphertext)
      - Store: [SHA3-256(recipient_kem_pk):32][kem_ct:1568][wrapped_data_key:48]
   e. AD = magic + version + suite + count + nonce + all_wrapped_keys_section

   Actually, this is getting complex. Let me simplify.

   SIMPLEST CORRECT approach:
   a. Generate random 32-byte data_key
   b. For each recipient:
      - kem_ct, kem_ss = encap_secret(recipient_kem_pk)
      - Use kem_ss to encrypt data_key:
        wrapped = aead_encrypt(data_key, b"", fixed_nonce, kem_ss)
        where fixed_nonce = b"\x00" * 12 (safe because kem_ss is unique per encap)
   c. Encrypt data: ciphertext = aead_encrypt(plaintext, AD, random_nonce, data_key)
   d. Assemble encrypted blob format

3. Sign and write (existing path):
   The encrypted_blob_bytes become the `data` field in write_blob().
   build_signing_input(namespace, encrypted_blob_bytes, ttl, timestamp)
   identity.sign(digest)
   encode_blob_payload(namespace, pubkey, encrypted_blob_bytes, ttl, timestamp, sig)
   send via transport

4. Server stores the blob as-is. Returns blob_hash + seq_num.
```

### Revised Encrypted Blob Format (After Data Flow Analysis)

```
+--------+-------+------------------------------------------+
| Offset | Size  | Field                                    |
+--------+-------+------------------------------------------+
| 0      | 4     | Magic: 0xE0 0xCE 0x10 0x70               |
| 4      | 1     | Version: 0x01                            |
| 5      | 1     | Algorithm suite: 0x01                    |
| 6      | 2     | Recipient count N (big-endian uint16)    |
| 8      | 12    | Data nonce (random)                      |
| 20     | N *   | Per-recipient wrapped key entries:        |
|        | 1648  |   [kem_pk_hash:32]                       |
|        |       |   [kem_ciphertext:1568]                  |
|        |       |   [wrapped_data_key:48]                  |
|        |       |   (48 = 32 data_key + 16 AEAD tag)       |
| 20+N*  | var   | AEAD ciphertext + 16-byte tag            |
| 1648   |       |                                          |
+--------+-------+------------------------------------------+
```

Each wrapped key entry is 1648 bytes (32 + 1568 + 48). The wrapped_data_key is the 32-byte data_key encrypted with ChaCha20-Poly1305 using the KEM shared secret as key and zero nonce (safe: KEM shared secret is unique per encapsulation, never reused).

**AD for data AEAD:** All bytes from offset 0 through end of wrapped keys section. This binds the ciphertext to the recipient list -- any modification to recipients invalidates the data ciphertext.

## Data Flow: Encrypted Read

```
Application code:
  result = await client.read_encrypted(namespace, blob_hash)

1. Read blob (existing path):
   read_result = await client.read_blob(namespace, blob_hash)
   Returns ReadResult(data=encrypted_blob_bytes, ttl, timestamp, signature)

2. Parse encrypted blob (_envelope.py):
   a. Check magic bytes at offset 0-3
   b. Read version, suite, recipient_count, nonce
   c. Scan wrapped key entries for SHA3-256(my_kem_pk)
   d. If not found: raise EnvelopeError("not a recipient")

3. Unwrap data key:
   a. Extract kem_ciphertext from my entry
   b. kem = oqs.KeyEncapsulation("ML-KEM-1024", secret_key=my_kem_sk)
   c. kem_ss = kem.decap_secret(kem_ciphertext)
   d. data_key = aead_decrypt(wrapped_data_key, b"", zero_nonce, kem_ss)
   e. If None: raise EnvelopeError("key unwrap failed")

4. Decrypt data:
   a. Reconstruct AD from header + all wrapped key entries
   b. plaintext = aead_decrypt(ciphertext, AD, data_nonce, data_key)
   c. If None: raise EnvelopeError("data decryption failed")

5. Return plaintext + metadata
```

## Directory Namespace Architecture

The directory is a regular chromatindb namespace owned by an organization admin. Users publish their public keys as blobs in this namespace via delegation.

### Directory Namespace Setup

```
Admin identity:
  signing_pk  -> namespace = SHA3-256(admin_signing_pk)
  This namespace IS the org directory.

For each user who should be in the org:
  1. Admin creates delegation: write_blob(data=DELEGATION_MAGIC + user_signing_pk)
     This grants user write access to the directory namespace.
  2. User self-registers: write_blob to admin's namespace with their key material.
```

### Directory Blob Types (New Magic Prefixes)

All directory data lives as blobs in the directory namespace. Distinguished by magic prefix:

```
USER_ENTRY_MAGIC  = 0xD1 0xC7 0x00 0x01   ("directory user entry v1")
GROUP_MAGIC       = 0xD1 0xC7 0x00 0x02   ("directory group v1")
```

**User Entry Blob Data Format:**
```
+--------+-------+------------------------------------------+
| Offset | Size  | Field                                    |
+--------+-------+------------------------------------------+
| 0      | 4     | Magic: USER_ENTRY_MAGIC                  |
| 4      | 1     | Version: 0x01                            |
| 5      | 1     | Label length (uint8, max 255)            |
| 6      | var   | Label (UTF-8 string, e.g., "alice")      |
| 6+L    | 2592  | ML-DSA-87 signing public key             |
| 6+L+   | 1568  | ML-KEM-1024 encryption public key        |
| 2592   |       |                                          |
+--------+-------+------------------------------------------+
Total: 4 + 1 + 1 + L + 2592 + 1568 = 4166 + L bytes
```

**Group Blob Data Format:**
```
+--------+-------+------------------------------------------+
| Offset | Size  | Field                                    |
+--------+-------+------------------------------------------+
| 0      | 4     | Magic: GROUP_MAGIC                       |
| 4      | 1     | Version: 0x01                            |
| 5      | 1     | Name length (uint8, max 255)             |
| 6      | var   | Group name (UTF-8, e.g., "engineering")  |
| 6+N    | 2     | Member count (big-endian uint16)          |
| 8+N    | var   | Per-member:                              |
|        |       |   [label_len:1][label:var]               |
+--------+-------+------------------------------------------+
```

Groups store member labels (not pubkeys). On encrypt, the SDK resolves each label to its KEM public key by scanning the directory. This keeps group blobs small and means group membership is always resolved at encrypt-time against the latest directory state.

### Directory Operations Mapped to Existing Client Methods

| Directory Operation | Underlying SDK Method | Notes |
|---|---|---|
| Create directory | `write_blob()` (admin identity) | First blob establishes namespace |
| Grant user access | `write_blob()` with DELEGATION_MAGIC data | Admin delegates write access |
| Self-register | `write_blob()` to admin namespace | User writes USER_ENTRY blob (delegated) |
| List users | `list_blobs()` + `read_blob()` per entry | Filter by USER_ENTRY_MAGIC prefix |
| Fetch user pubkeys | `read_blob()` on specific user entry | Parse USER_ENTRY format |
| Create group | `write_blob()` with GROUP_MAGIC data | Admin or delegated user writes group blob |
| Update group | `delete_blob()` old + `write_blob()` new | Tombstone + replace pattern |
| List groups | `list_blobs()` + `read_blob()` per entry | Filter by GROUP_MAGIC prefix |
| Revoke user | `delete_blob()` delegation + `delete_blob()` user entry | Tombstone both |

**No new wire types. No server changes. Everything is blobs.**

### Directory Caching

`DirectoryManager` should cache user entries (pubkeys) in memory during a session. The KEM public keys (1568 bytes each) are large, and fetching them for every encrypt is wasteful.

Cache invalidation: subscribe to the directory namespace. When a Notification arrives, invalidate the relevant cache entry. Simple, reactive, built on existing pub/sub.

```python
class DirectoryManager:
    def __init__(self, client: ChromatinClient, admin_namespace: bytes):
        self._client = client
        self._admin_namespace = admin_namespace
        self._user_cache: dict[str, UserEntry] = {}  # label -> UserEntry
        self._group_cache: dict[str, Group] = {}      # name -> Group
```

## Identity Extension for KEM

The existing `Identity` class manages ML-DSA-87 only. For v1.7.0, it needs an optional ML-KEM-1024 keypair.

### Key File Layout

```
Current (v1.6.0):
  mykey.key   (4896 bytes, ML-DSA-87 secret key)
  mykey.pub   (2592 bytes, ML-DSA-87 public key)

Extended (v1.7.0, backward compatible):
  mykey.key   (4896 bytes, ML-DSA-87 secret key)     [unchanged]
  mykey.pub   (2592 bytes, ML-DSA-87 public key)     [unchanged]
  mykey.kem   (3168 bytes, ML-KEM-1024 secret key)   [NEW, optional]
  mykey.kpub  (1568 bytes, ML-KEM-1024 public key)   [NEW, optional]
```

Backward compatibility: `Identity.load()` checks for `.kem`/`.kpub` files. If absent, `has_kem` is False and encryption features are unavailable. `Identity.generate()` always creates both keypairs going forward.

### Identity API Changes

```python
class Identity:
    # Existing (unchanged):
    @property
    def public_key(self) -> bytes: ...       # ML-DSA-87 (2592 bytes)
    @property
    def namespace(self) -> bytes: ...        # SHA3-256(signing_pk)
    @property
    def can_sign(self) -> bool: ...
    def sign(self, message: bytes) -> bytes: ...
    @staticmethod
    def verify(message, signature, public_key) -> bool: ...

    # New:
    @property
    def kem_public_key(self) -> bytes | None: ...   # ML-KEM-1024 (1568 bytes)
    @property
    def kem_secret_key(self) -> bytes | None: ...   # ML-KEM-1024 (3168 bytes)
    @property
    def has_kem(self) -> bool: ...

    @classmethod
    def generate(cls) -> Identity:
        # Now generates BOTH ML-DSA-87 + ML-KEM-1024 keypairs

    @classmethod
    def from_public_keys(cls, signing_pk: bytes, kem_pk: bytes | None = None) -> Identity:
        # Verify-only identity, optionally with KEM public key for encrypting TO them
```

## Component Boundaries

| Component | Responsibility | Depends On |
|---|---|---|
| `_envelope.py` | Encrypt/decrypt blob data using KEM envelope scheme | `crypto.py` (AEAD, HKDF, SHA3-256), `oqs` (ML-KEM-1024) |
| `_directory.py` | User/group CRUD in directory namespace, caching | `client.py` (read/write/list/delete/subscribe), `_envelope.py`, `_directory_types.py` |
| `_directory_types.py` | Frozen dataclasses for directory entries | None (pure data) |
| `identity.py` (modified) | ML-DSA-87 + ML-KEM-1024 keypair management | `oqs`, `crypto.py` |
| `client.py` (modified) | High-level encrypt/decrypt convenience methods | `_envelope.py`, `_directory.py` |

### Dependency Graph

```
client.py
  |
  +-- _directory.py
  |     |
  |     +-- _envelope.py
  |     |     |
  |     |     +-- crypto.py (AEAD, HKDF, SHA3)
  |     |     +-- oqs (ML-KEM-1024)
  |     |
  |     +-- _directory_types.py
  |     +-- _codec.py (existing encode/decode)
  |     +-- types.py (existing result types)
  |
  +-- _envelope.py (also used directly for low-level encrypt/decrypt)
  +-- identity.py (extended with KEM)
```

## Anti-Patterns to Avoid

### Anti-Pattern 1: Putting KEM Keypair in a Separate Class
**What:** Creating `EncryptionIdentity` or `KEMIdentity` separate from `Identity`.
**Why bad:** Users already have Identity objects. Forcing them to manage two separate identity objects for signing vs encryption creates confusion and doubles key management code.
**Instead:** Extend the existing Identity class with optional KEM fields. One identity, two purposes.

### Anti-Pattern 2: Encrypting the FlatBuffer Instead of the Data
**What:** Encrypting the entire FlatBuffer blob payload and storing that.
**Why bad:** The node needs to verify the signature on the FlatBuffer. If the entire FlatBuffer is encrypted, the node cannot verify ownership and will reject the blob.
**Instead:** Encrypt only the application data. The `data` field of the FlatBuffer contains the encrypted blob format. The FlatBuffer itself (with namespace, pubkey, signature) remains in the clear for node verification.

### Anti-Pattern 3: Modifying the Node for Encrypted Blobs
**What:** Adding encryption awareness to the C++ node.
**Why bad:** The node is intentionally dumb. Client-side encryption means the node NEVER needs to know about encryption. Adding node-side logic couples layers and defeats zero-knowledge storage.
**Instead:** All encryption/decryption is in the SDK. The node sees opaque `data` bytes.

### Anti-Pattern 4: Storing KEM Public Keys in a Separate Namespace
**What:** Creating a special `_keys` namespace for public key storage.
**Why bad:** Requires a separate identity to own `_keys`. Who owns it? How is it bootstrapped? Adds unnecessary complexity.
**Instead:** Store user entries (including KEM public keys) in the org admin's directory namespace. The admin owns it, delegates write access to users. Uses existing delegation primitive.

### Anti-Pattern 5: Group Blobs Containing Full Public Keys
**What:** Storing all member KEM public keys inside the group blob.
**Why bad:** Each ML-KEM-1024 public key is 1568 bytes. A 50-member group would be 78KB for the group blob alone. Group updates require re-writing all keys.
**Instead:** Store member labels in the group blob. Resolve labels to KEM public keys at encrypt-time by reading user entries from the directory. Keeps group blobs tiny.

## Patterns to Follow

### Pattern 1: Magic Prefix Dispatch
**What:** Use 4-byte magic prefixes to identify blob content type.
**When:** Whenever a new blob type is introduced.
**Why:** Consistent with existing TOMBSTONE_MAGIC and DELEGATION_MAGIC patterns. Any code can identify blob type without external metadata.

```python
ENCRYPTED_BLOB_MAGIC = bytes([0xE0, 0xCE, 0x10, 0x70])
USER_ENTRY_MAGIC     = bytes([0xD1, 0xC7, 0x00, 0x01])
GROUP_MAGIC          = bytes([0xD1, 0xC7, 0x00, 0x02])
```

### Pattern 2: Blob-as-Record
**What:** All structured data stored as blobs with magic prefix + version + fields.
**When:** Storing user entries, groups, encrypted data.
**Why:** No new infrastructure needed. Leverages existing signed blob storage, replication, and query. Everything is blobs.

### Pattern 3: Tombstone + Replace for Updates
**What:** To update a record (e.g., group membership), tombstone the old blob and write a new one.
**When:** Group membership changes, user key rotation.
**Why:** chromatindb is append-only with tombstones. There is no in-place update. Tombstone + write is the canonical mutation pattern.

### Pattern 4: Subscribe for Cache Invalidation
**What:** Subscribe to directory namespace, invalidate cache entries on notifications.
**When:** DirectoryManager is active and caching user/group data.
**Why:** Reactive cache invalidation using existing pub/sub. No polling needed.

### Pattern 5: Zero-Nonce for Unique-Key AEAD
**What:** Use all-zeros nonce when the AEAD key is unique and never reused.
**When:** Wrapping data_key with KEM shared secret. Each KEM encapsulation produces a unique shared secret, so the nonce never repeats for a given key.
**Why:** Simpler than deriving a nonce. Cryptographically safe because the key uniqueness guarantee comes from KEM.

## Suggested Build Order

The build order follows dependency depth -- build leaves first, then composites.

### Phase 1: Identity Extension + Envelope Crypto
**Build:** Extend `identity.py` with KEM keypair, create `_envelope.py`
**Rationale:** No network IO needed. Purely crypto. Fully testable with unit tests.
**Dependencies:** `crypto.py` (existing), `oqs` (existing dep)
**Delivers:** `Identity` with KEM + `envelope_encrypt()`/`envelope_decrypt()` + encrypted blob format

### Phase 2: Directory Types + Directory Manager (Read Path)
**Build:** Create `_directory_types.py`, create `_directory.py` with user/group encode/decode + discovery (read-only)
**Rationale:** Depends on Phase 1 (UserEntry contains KEM pubkey). Read path is simpler than write.
**Dependencies:** Phase 1, `client.py` (existing read/list methods)
**Delivers:** Parse user entries and groups from blobs, list users, fetch pubkeys, resolve groups to KEM pubkeys

### Phase 3: Directory Manager (Write Path) + Client Integration
**Build:** Add directory write operations (register, create group, update group, revoke) + `write_encrypted()`/`read_encrypted()` on ChromatinClient
**Rationale:** Depends on Phase 2 (directory read for recipient resolution). Full integration.
**Dependencies:** Phase 1, Phase 2
**Delivers:** Complete encrypted write/read flow, user self-registration, group management

### Phase 4: Polish + Documentation
**Build:** Cache with pub/sub invalidation, error handling edge cases, README, tutorial updates
**Rationale:** Polish after core is working. Cache is an optimization, not critical path.
**Dependencies:** Phase 3
**Delivers:** Production-ready encrypted storage with caching, documentation

## Scalability Considerations

| Concern | At 10 users | At 100 users | At 1000 users |
|---|---|---|---|
| Encrypted blob size overhead | 1 recipient: ~1.7KB header | 10 recipients: ~16.5KB header | 100 recipients: ~165KB header. Acceptable for most blob sizes. |
| Directory scan for user lookup | Trivial: list_blobs returns <10 entries | Moderate: may need pagination | Consider label-indexed lookup blob. Scan is O(N) per page. |
| Group resolution | Trivial | Cache eliminates repeated lookups | Same -- cache handles it |
| KEM encapsulation per recipient | ~1ms each, 10ms total | ~100ms | ~1s. CPU-bound. Acceptable for write path. |
| Identity file size | 12.8KB (DSA + KEM keys) | Same per user | Same per user |

The 1000-user case for directory scan could be optimized later with an index blob that maps labels to blob hashes, but this is YAGNI for v1.7.0. The list_blobs pagination handles it adequately.

## Sources

- ML-KEM-1024 key sizes: [Open Quantum Safe ML-KEM docs](https://openquantumsafe.org/liboqs/algorithms/kem/ml-kem.html) -- PUBLIC_KEY=1568, SECRET_KEY=3168, CT=1568, SS=32
- liboqs-python KEM API: [GitHub liboqs-python](https://github.com/open-quantum-safe/liboqs-python) -- KeyEncapsulation(alg, secret_key=), generate_keypair(), encap_secret(pk), decap_secret(ct), export_secret_key()
- FIPS 203 ML-KEM standard: [NIST FIPS 203](https://csrc.nist.gov/pubs/fips/203/final)
- KEM envelope pattern: [Wikipedia KEM](https://en.wikipedia.org/wiki/Key_encapsulation_mechanism) + HPKE draft
- Existing SDK source: Direct analysis of all 12 modules under sdk/python/chromatindb/
- Delegation format: db/wire/codec.h DELEGATION_MAGIC (0xDE1E6A7E) + 2592-byte pubkey
- Tombstone format: db/wire/codec.h TOMBSTONE_MAGIC (0xDEADBEEF) + 32-byte target hash
