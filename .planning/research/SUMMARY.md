# Project Research Summary

**Project:** chromatindb v1.7.0 — Client-Side PQ Envelope Encryption
**Domain:** Post-quantum envelope encryption with pubkey directory and group management in a Python SDK
**Researched:** 2026-03-31
**Confidence:** HIGH

## Executive Summary

v1.7.0 adds client-side PQ envelope encryption to the existing Python SDK. The feature set is well-understood: envelope encryption is a mature pattern (age, AWS KMS, CMS) and every required cryptographic primitive is already present in the SDK dependency set — no new pip packages are needed. The implementation is entirely SDK-side; the C++ node remains a zero-knowledge store with no changes required.

The recommended approach is standard KEM-DEM (Key Encapsulation Mechanism — Data Encapsulation Mechanism): generate a random per-blob data encryption key (DEK), encrypt the blob data once with ChaCha20-Poly1305, then for each recipient use ML-KEM-1024 `encap_secret()` to obtain a per-recipient shared secret, derive a key-encryption key (KEK) via HKDF, and wrap the DEK with the KEK. The pubkey directory is implemented as ordinary signed blobs in an admin-owned namespace — no new infrastructure, no new wire types, no node changes. Groups are named member lists stored as blobs; group encryption resolves members at encrypt-time and wraps the DEK individually for each.

The primary risks are all cryptographic design risks, not engineering risks. The most critical: ML-KEM is NOT RSA — the caller cannot choose what value gets encapsulated, so the two-layer KEM-then-Wrap pattern is mandatory for multi-recipient support. Secondary risks are nonce management (data AEAD must use random nonces, not the transport layer's counter), missing version bytes in the envelope format (locks in an unmigrateable format), and binding the published KEM encapsulation key to the signing identity (prevents man-in-the-middle key substitution). All of these are preventable by locking design decisions in the first phase plan before any code is written.

## Key Findings

### Recommended Stack

The v1.7.0 stack is identical to v1.6.0 — zero new dependencies. Every capability needed for envelope encryption is already present: ML-KEM-1024 via liboqs-python (already exercised in the handshake), ChaCha20-Poly1305 AEAD via PyNaCl (already in `crypto.py`), HKDF-SHA256 via stdlib `hmac`/`hashlib` (already in `_hkdf.py`), and binary format encoding via stdlib `struct` (already the pattern throughout `_codec.py`). The only new stdlib usage is `secrets.token_bytes()` for DEK and nonce generation, which is available since Python 3.6 and is the canonical PEP 506 approach to cryptographic randomness.

**Core technologies (unchanged from v1.6.0):**
- `liboqs-python ~=0.14.0`: ML-KEM-1024 `encap_secret()` / `decap_secret()` for per-recipient key wrapping — already proven in `_handshake.py`
- `pynacl ~=1.5.0`: ChaCha20-Poly1305 AEAD for DEK wrapping and blob data encryption — existing `aead_encrypt`/`aead_decrypt` functions
- `flatbuffers ~=25.12`: Blob payload encoding for network writes — envelope ciphertext stored as opaque bytes in the `data` field, FlatBuffers unchanged
- `secrets` (stdlib): `secrets.token_bytes(32)` for DEK, `secrets.token_bytes(12)` for AEAD nonce — new usage, zero-dep
- `struct` (stdlib): Binary envelope format encode/decode — same pattern as all existing `_codec.py` work

**pyproject.toml `dependencies` list:** no changes needed.

### Expected Features

The feature set follows the age/CMS multi-recipient envelope encryption pattern adapted to the PQ stack.

**Must have (table stakes):**
- Envelope encryption format — self-describing binary header with version, recipient count, per-recipient wrapped keys, then AEAD ciphertext. Must be parseable without external state.
- Multi-recipient key wrapping — single DEK, encrypted once; per-recipient KEM encapsulation + AEAD-wrapped DEK. One DEK for N recipients or the data would need to be encrypted N times.
- Identity with ML-KEM-1024 keypair — extend existing `Identity` class with optional `.kem` / `.kpub` files alongside existing `.key` / `.pub`. One identity, two keypairs (sign + encrypt).
- Pubkey directory — user KEM pubkeys stored as signed blobs in an admin-owned namespace. Self-registration via delegation. No new infrastructure.
- `write_encrypted()` / `read_encrypted()` on `ChromatinClient` — thin wrappers composing envelope crypto with existing `write_blob`/`read_blob`.

**Should have (differentiators):**
- Named groups in directory — group blob stores member labels; SDK resolves to KEM pubkeys at encrypt-time. Eliminates per-blob recipient enumeration.
- `write_to_group()` helper — one-liner for the common case.
- Self-encrypting write (no explicit recipients = encrypt to self only).
- Directory key caching with pub/sub invalidation — avoids re-fetching on every encrypt.

**Defer to v2+:**
- Streaming encryption for large blobs (STREAM construction). Only needed if encrypted 100 MiB blobs become common; per-blob in-memory encryption is fine for typical usage.
- Re-encryption helper for revocation. O(blobs x members), expensive, manual is fine for v1.7.0.
- Client-side dedup before encryption (requires client-side state tracking).

**Anti-features (never build):**
- Shared group symmetric key — requires key rotation on every membership change; per-blob envelope encryption is simpler and equally secure for a blob store.
- Key escrow — destroys zero-knowledge property.
- Convergent encryption — leaks plaintext equality, classic chosen-plaintext attack.
- Automatic re-encryption on group membership change — scales catastrophically with data volume.

### Architecture Approach

The feature is implemented in three new modules plus targeted extensions to two existing modules. `_envelope.py` handles pure crypto (encrypt/decrypt, format encode/decode) with no network IO — fully unit-testable in isolation. `_directory.py` wraps `ChromatinClient` to manage user and group CRUD in a directory namespace, composing envelope crypto with existing `read_blob`/`write_blob`/`list_blobs`/`subscribe` methods. `_directory_types.py` holds frozen dataclasses for directory entries. The C++ node is never touched — all encryption is opaque bytes in the `data` field of a standard signed blob.

**Major components:**
1. `_envelope.py` — Encrypt blob data with per-blob random DEK; ML-KEM-1024 encap/decap per recipient; binary format encode/decode. Depends on `crypto.py` and `oqs`.
2. `_directory.py` — `DirectoryManager` class: user registration, user discovery, group CRUD, group resolution, in-memory cache with pub/sub invalidation. Depends on existing client + envelope.
3. `_directory_types.py` — Frozen dataclasses: `UserEntry`, `Group`, `EncryptedBlob`, `DirectoryConfig`.
4. `identity.py` (extended) — Add optional ML-KEM-1024 keypair fields, `.kem`/`.kpub` file persistence, backward-compatible `load()`.
5. `client.py` (extended) — Add `write_encrypted()`, `read_encrypted()` convenience methods.

**Encrypted blob format (binary, big-endian):**
```
[magic:4][version:1][suite:1][recipient_count:2 BE][data_nonce:12]
[N x (kem_pk_hash:32 + kem_ciphertext:1568 + wrapped_dek:48)]
[ciphertext + 16-byte AEAD tag]
```
Per-recipient overhead: 1648 bytes fixed. Fixed header: 20 bytes + 16-byte AEAD tag.

**Directory namespace:** Admin-owned, users self-register via delegation (existing primitive). User entry blobs: `USER_ENTRY_MAGIC` + signing pubkey + KEM pubkey + label + signature. Group blobs: `GROUP_MAGIC` + name + member labels. No new wire types.

**Key patterns to follow:**
- Magic prefix dispatch (4-byte magic at offset 0) — consistent with `TOMBSTONE_MAGIC`, `DELEGATION_MAGIC`
- Tombstone + replace for updates (chromatindb is append-only; group membership changes use this pattern)
- Subscribe for cache invalidation (existing pub/sub, no polling)
- Zero nonce for unique-key AEAD (wrapping DEK with KEM shared secret — key is unique per encapsulation)

### Critical Pitfalls

1. **ML-KEM is a KEM, not RSA-style encryption** — `encap_secret()` returns a random shared secret the caller cannot choose. The mandatory two-layer pattern: random DEK -> per-recipient `encap_secret()` -> derive KEK via HKDF -> wrap DEK with KEK via AEAD. No HKDF step or a counter-nonce from transport reused here means broken multi-recipient encryption.

2. **Nonce reuse between transport AEAD and data AEAD** — Transport uses counter-based nonces (stateful, session-scoped). Data AEAD must use `secrets.token_bytes(12)` random nonces. A per-blob fresh DEK means even accidental nonce collision wouldn't reuse the same (key, nonce) pair, but relying on that is fragile. Random nonces are required.

3. **Missing version byte in envelope format** — Encrypted blobs without a plaintext version byte cannot be migrated when ML-KEM-1024 is superseded (HQC, or a version bump). The format must be frozen with a version byte before any blobs are written to storage. Changing the format afterwards requires migration tooling.

4. **Missing HKDF domain separation** — Three HKDF domains exist: transport (`chromatin-init-to-resp-v1`, `chromatin-resp-to-init-v1`), DARE (`chromatindb-dare-v1`), envelope KEK (`chromatindb-envelope-kek-v1`). Each must have a unique context label. Reuse is a silent cryptographic failure.

5. **No signature binding between signing and encryption keys** — Users publish ML-KEM-1024 encapsulation keys to the directory. Without signing these with the ML-DSA-87 identity key, a MITM can substitute their own encapsulation key. Every `UserEntry` must include an ML-DSA-87 signature over the KEM public key.

## Implications for Roadmap

The research is clear on phase ordering: build leaves before composites, pure crypto before network, format before helpers.

### Phase 1: Identity Extension + Envelope Crypto

**Rationale:** Foundation for everything else. No network IO — fully unit-testable. The envelope format is a locked design decision that cannot change after blobs are written to storage. KEM keypair persistence must exist before any directory or encryption helpers are built.
**Delivers:** Extended `Identity` with ML-KEM-1024 keypair (generate, save, load, backward-compatible); `_envelope.py` with `envelope_encrypt()` / `envelope_decrypt()`; encrypted blob binary format (magic, version byte, suite byte, multi-recipient wrapped keys, AEAD ciphertext with authenticated header).
**Addresses:** Envelope encryption format (table stakes), Identity with KEM keypair (table stakes).
**Avoids:** Pitfall 1 (KEM misuse), Pitfall 4 (nonce reuse), Pitfall 5 (missing version byte), Pitfall 9 (HKDF domain separation), Pitfall 10 (keypair lifecycle).

### Phase 2: Directory Types + Directory Read Path

**Rationale:** Depends on Phase 1 (UserEntry contains KEM pubkey — format must be defined first). Read path is simpler than write and proves the format works before any data is committed. Separating read from write reduces integration surface per phase.
**Delivers:** `_directory_types.py` frozen dataclasses; `_directory.py` user/group blob format encode/decode; `DirectoryManager` list users, fetch KEM pubkeys, resolve groups to KEM pubkeys; in-memory cache with TTL.
**Addresses:** Pubkey directory (table stakes), user discovery.
**Avoids:** Pitfall 7 (encapsulation key validation — verify size and signature before use), Pitfall 6 (eventual consistency — cache with TTL, not indefinitely).

### Phase 3: Directory Write Path + Client Integration

**Rationale:** Depends on Phase 2 (read path must exist for recipient resolution). Completes the full encrypt-write and read-decrypt cycle. Group management builds on directory read infrastructure.
**Delivers:** User self-registration (`DirectoryManager.register()`); group CRUD (create, add member, remove member, list); `ChromatinClient.write_encrypted()` / `read_encrypted()`; `write_to_group()` helper; pub/sub cache invalidation.
**Addresses:** `write_encrypted` / `read_encrypted` (table stakes), groups (differentiator), group management, self-registration.
**Avoids:** Pitfall 8 (group membership races — snapshot semantics, accept over/under-encryption as correct, record group version in envelope), Pitfall 3 (content-addressing breaks for encrypted blobs — explicit documented design decision).

### Phase 4: Documentation + Polish

**Rationale:** Polish after core is working. Error handling edge cases, README encryption API section, tutorial updates, and PROTOCOL.md with HKDF label registry and envelope format spec.
**Delivers:** README encryption section; tutorial updates; PROTOCOL.md envelope format + HKDF label registry; explicit error handling for malformed envelopes, not-a-recipient, stale keys.

### Phase Ordering Rationale

- Phase 1 before Phase 2 because `UserEntry` contains a KEM public key — Identity KEM extension and envelope format must be defined first.
- Phase 2 read-only before Phase 3 write because self-registration requires correct format encode/decode proven before writing directory entries to live storage.
- Groups (Phase 3) after directory read (Phase 2) because group resolution requires fetching member KEM pubkeys from the directory.
- Documentation last (Phase 4) because the API surface must be stable before writing tutorials.
- The encrypted blob format must be frozen in Phase 1 before any blobs are written. Retroactively changing the format requires migration tooling and a flag day.

### Research Flags

Phases likely needing deeper research during planning:
- **None.** The research is comprehensive and all design decisions are resolved. The PITFALLS.md covers every sharp edge with specific prevention strategies and code patterns. The ARCHITECTURE.md provides complete build order, component boundaries, and data flows.

Phases with standard patterns (skip `/gsd:research-phase`):
- **All four phases** — domain is well-documented, cryptographic patterns are established (KEM-DEM, HKDF domain separation, blob-as-record), and the existing SDK architecture provides clear extension points for every new component.

## Confidence Assessment

| Area | Confidence | Notes |
|------|------------|-------|
| Stack | HIGH | Zero new dependencies confirmed. All API usage verified against liboqs-python source and existing SDK tests. ML-KEM-1024 key sizes confirmed against FIPS 203. |
| Features | HIGH | Envelope encryption pattern well-documented (age, CMS, AWS KMS). PQ-specific integration covered by IETF KEM drafts and existing SDK handshake code. |
| Architecture | HIGH | All existing modules analyzed directly. Component boundaries are clear. New modules have no circular dependencies. Format design verified against existing `_codec.py` conventions. |
| Pitfalls | HIGH | Pitfalls verified against FIPS 203, IETF ML-KEM security considerations, CMS-KEM spec (RFC 9629), and existing codebase. Phase-to-pitfall mapping complete. |

**Overall confidence:** HIGH

### Gaps to Address

- **Wrap nonce for DEK wrapping:** ARCHITECTURE.md settled on zero nonce (safe because KEM shared secret is unique per encapsulation). PITFALLS.md Pitfall 5 suggests a separate wrap nonce field may be clearer. The Phase 1 plan should explicitly lock this decision and add a code comment explaining why zero nonce is safe in this context.
- **liboqs internal key validation:** PITFALLS.md Pitfall 7 notes that FIPS 203 requires an Encapsulation Key Check, and it is unclear whether liboqs-python performs this check internally during `encap_secret()`. The Phase 1 plan should verify this against liboqs source and add an explicit size + structure check if liboqs does not handle it.
- **Signature field in UserEntry:** FEATURES.md and PITFALLS.md both require that each published KEM public key be signed with the user's ML-DSA-87 identity. ARCHITECTURE.md's `UserEntry` format does not explicitly include this field. The Phase 2 plan must include the signature field in the user entry blob format specification.

## Sources

### Primary (HIGH confidence)
- liboqs-python GitHub / oqs.py source — `KeyEncapsulation` API, key sizes, `encap_secret()` / `decap_secret()` / `generate_keypair()` / `export_secret_key()` methods
- OQS ML-KEM docs (openquantumsafe.org) — ML-KEM-1024 parameters: pk=1568, sk=3168, ct=1568, ss=32, NIST Level 5
- NIST FIPS 203 — ML-KEM-1024 Encapsulation Key Check, algorithm specification
- age encryption specification (C2SP) — multi-recipient stanzas, KEM-DEM pattern, STREAM payload construction
- IETF RFC 5869 HKDF — key derivation for KEM shared secrets
- Existing SDK source (sdk/python/chromatindb/, all 12 modules) — extension point analysis
- Existing C++ source (db/storage/storage.cpp, db/crypto/master_key.h) — DARE format, HKDF label registry

### Secondary (MEDIUM confidence)
- IETF draft-sfluhrer-cfrg-ml-kem-security-considerations-01 — "ML-KEM is not a drop-in replacement for RSA-KEM"
- IETF draft-ietf-lamps-cms-kyber-10 / RFC 9629 — CMS KEMRecipientInfo with HKDF + AEAD-Wrap pattern
- IETF draft-ietf-jose-pqc-kem — KEM envelope encryption design patterns
- Google Cloud envelope encryption docs — DEK/KEK two-layer pattern
- Keybase KBFS crypto spec / teams design — group key distribution and re-encryption tradeoffs
- Neil Madden blog (multi-recipient KEM-DEM, Tag-KEMs) — KEM-DEM architecture analysis
- NIST SP 800-227 — Recommendations for Key-Encapsulation Mechanisms

### Tertiary (LOW confidence)
- Azure Blob Storage client-side encryption migration — format versioning migration pain (illustrative)
- Secure Deduplication of Encrypted Data (ePrint 2017/1089) — convergent encryption weaknesses

---
*Research completed: 2026-03-31*
*Ready for roadmap: yes*
