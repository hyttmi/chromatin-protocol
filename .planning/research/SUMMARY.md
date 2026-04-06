# Project Research Summary

**Project:** chromatindb v2.1.1 — Revocation & Key Lifecycle
**Domain:** ACL revocation, envelope key versioning, group membership revocation in decentralized PQ-encrypted eventually-consistent blob storage
**Researched:** 2026-04-06
**Confidence:** HIGH

## Executive Summary

This milestone adds access lifecycle management to chromatindb: revoking delegate write access, rotating KEM keypairs for envelope encryption, and excluding removed group members from future ciphertexts. The headline finding across all four research areas is that the existing primitives already implement 90% of the required machinery. The node-layer delegation revocation mechanism (tombstone delegation blob -> `delegation_map` entry erased -> subsequent writes rejected) is fully implemented, tested, and proven in Docker integration tests. The per-blob envelope encryption model (no shared group key, per-recipient KEM wrapping) was specifically chosen in v1.7.0 to avoid the key rotation complexity that plagues group messaging protocols. The milestone is purely additive SDK work with no new wire types, no C++ node changes, and no new dependencies.

The recommended approach is a four-phase SDK-only build: delegation revocation helper first (smallest scope, highest immediate value), then KEM key versioning with envelope format extension (technically deepest, independent of phase 1), then group membership revocation (straightforward once key versioning patterns are established), and protocol documentation last. The critical design boundary is that old data deliberately remains readable by old keyholders — this is both a project constraint and the universal industry pattern (AWS KMS, GCP KMS, Signal, MLS all follow it). Every feature decision should reinforce this boundary, not accidentally work around it.

The primary risk is scope creep: key versioning has a well-documented "pull thread" problem where each design decision creates new questions toward a full KMS. The research defines a hard scope boundary: `key_version` is an integer in the envelope header and an array of KEM keypairs in the identity file. Nothing more. A secondary risk is misunderstanding the propagation window for revocation — tombstone replication is eventually consistent (sub-second for connected peers, up to safety-net interval for disconnected ones), not atomic. This must be documented, not engineered away.

## Key Findings

### Recommended Stack

No dependency changes required. The existing stack (liboqs ML-DSA-87 + ML-KEM-1024, libsodium ChaCha20-Poly1305 + HKDF-SHA256, libmdbx) is sufficient for all three feature areas. ACL revocation uses ML-DSA-87 signing of tombstone blobs (already implemented). KEM key versioning uses ML-KEM-1024 keygen (already in `identity.py`) with existing HKDF label `chromatindb-envelope-kek-v1` (unchanged — the KEM shared secret is unique per encapsulation, not per label). Group membership revocation requires zero crypto changes because per-blob DEK wrapping with per-recipient KEM stanzas already handles "exclude this member from future encryptions" naturally.

The "no new dependencies" constraint is not a limitation — it is a confirmation that the original stack was well-chosen. All crypto operations needed for revocation and key lifecycle are direct reuse of primitives already validated in production.

**Core technologies (unchanged):**
- liboqs ML-DSA-87: signing revocation tombstones and cross-signing rotated KEM pubkeys — same operations as delegation creation
- liboqs ML-KEM-1024: keypair generation on rotation + encapsulation to latest key per recipient — `generate_keypair()` already exists
- libsodium ChaCha20-Poly1305: AEAD for envelope DEK wrapping — KEK is unique per encapsulation regardless of key version
- libsodium HKDF-SHA256: key derivation — label `chromatindb-envelope-kek-v1` stays the same; do NOT create a `chromatindb-envelope-kek-v2` label
- libmdbx: `delegation_map` already tracks active delegations; tombstones already erase entries on receipt

**What NOT to use:** No shared group symmetric keys, no re-encryption on revocation, no new cipher suites for key versions, no ENVELOPE_VERSION bump for key lifecycle changes (version byte is for format changes, not key generations), no OpenSSL, no external KMS.

### Expected Features

The feature set divides into six items that close the revocation story (P1) and three enhancements for post-validation (P2/P3).

**Must have (table stakes):**
- SDK delegation revocation helper — `Directory.revoke_delegation(delegate_identity)` locates the delegation blob hash via DelegationListRequest (already on wire, types 51/52) and tombstones it. Node enforcement already works; this is a missing API surface.
- Admin KEM key rotation — `Identity.rotate_kem()` generates new ML-KEM-1024 keypair, retains old keypair in a key ring, then publishes a new UserEntry v2 blob to the directory. Without rotation, a compromised KEM secret key enables decryption of all future data addressed to that identity.
- Envelope key versioning (UserEntry v2 + envelope header extension) — `key_version` field in UserEntry blob and in the envelope plaintext header as AEAD additional data. Enables O(1) KEM key selection on decrypt. Old envelopes remain decryptable via key ring lookup by `kem_pk_hash`.
- Group membership revocation — `remove_member()` already excludes revoked member from future `write_to_group()` calls. Needs: GroupEntry v2 format with `version_counter` for epoch tracking, and a cache freshness check before encryption after removal.
- Old data readable with old keys — `envelope_decrypt()` falls back to key ring when current KEM key does not match stanza `kem_pk_hash`. Identity retains all historical KEM keypairs indexed by `SHA3-256(kem_pk)`.
- PROTOCOL.md update — UserEntry v2 format, envelope header extension, revocation semantics, propagation window bounds.

**Should have (differentiators):**
- Atomic revoke-and-rotate — single `Directory.revoke_member(group_name, member)` call that tombstones delegation, removes from group, and rotates admin KEM key atomically. Prevents the window between revocation and key rotation.
- Revocation propagation notification — already works via existing pub/sub on directory namespace; needs documentation only (zero new code).
- Key history chain — directory cache stores list of `(key_version, kem_pubkey)` per user; walk history to resolve any past epoch's KEM pubkey for old envelope decryption.

**Defer (v2+):**
- Periodic automatic key rotation (timer-based, compliance use case only; no value without an active revocation event)
- Re-encryption utility for old data under new keys (expensive, violates constraint, only needed for extreme compliance scenarios)
- Cross-SDK key versioning (Python SDK is the only SDK today)
- Permanent ban semantics via revocation list blob (prevents re-delegation; tombstone-based revocation allows re-delegation by design)

### Architecture Approach

All components are SDK-only Python changes to four existing modules. The C++ node requires zero modifications: tombstone-based delegation revocation is already implemented and tested (`test_acl04_revocation.sh`), and the node stores opaque signed blobs without interpreting envelope encryption or group membership. The four architectural components are: (1) `Directory.revoke_delegation()` using existing DelegationList + delete_blob primitives, (2) `Identity` key ring with multi-version KEM storage and `rotate_kem()` method, (3) envelope header extended with `key_version: uint32 BE` as authenticated plaintext in AEAD additional data, and (4) `GroupEntry` v2 format adding `version_counter: uint32 BE` for membership epoch tracking. No new MDBX sub-databases, no new wire message types, no new files required.

**Major components:**
1. `_directory.py` — add `revoke_delegation()`, `list_delegates()`, UserEntry v2 encode/decode, GroupEntry v2 encode/decode with version_counter, atomic `revoke_member()` orchestration
2. `identity.py` — add `rotate_kem()`, key ring persistence (`_kem_ring: dict[bytes, KeyEncapsulation]` indexed by pk_hash), updated `save()`/`load()` for multi-keypair storage
3. `_envelope.py` — extend header with `key_version: uint32 BE` as AEAD additional data, add key ring fallback in `envelope_decrypt()` on `NotARecipientError`
4. `client.py` — add `revoke_delegation()` convenience wrapper, expose atomic `revoke_member()` method

### Critical Pitfalls

1. **key_version in encrypted payload instead of header** — if `key_version` is placed inside the AEAD ciphertext, readers must trial-decrypt with every key version (O(versions) KEM decap attempts; unusable beyond 5 rotations). Put `key_version` in the plaintext header as AEAD additional data for O(1) key selection. Design the extended envelope header format before writing any rotation logic.

2. **Key rotation without re-registration** — generating a new KEM keypair locally without publishing a new UserEntry blob causes all other senders to encrypt with the stale old pubkey. The rotating user cannot decrypt. Make `rotate_kem()` atomic with directory re-registration — a single SDK call with no way to do one without the other.

3. **Key rotation discards old KEM secret key** — after rotation, all data encrypted under the old key becomes permanently unreadable (catastrophic data loss). `Identity.rotate_kem()` must move the old KEM keypair to `_kem_ring`, never delete it. Identity save/load must persist the full ring.

4. **Revocation propagation window misrepresented as instant** — tombstone replication is eventually consistent. Connected peers receive tombstones in near-real-time via BlobNotify. Disconnected peers may not receive them until the safety-net interval (default 600s). Document the propagation bounds explicitly in PROTOCOL.md. Do not add tests with fixed sleep durations. Do not add a consensus protocol.

5. **Tombstone TTL ever made non-zero** — the existing ingest pipeline correctly blocks delegation blobs whose tombstone arrived first (step 3.5 tombstone check before store). If delegation tombstones ever expire, a delayed delegation blob arriving after expiry repopulates `delegation_map` with a revoked delegate. Delegation revocation tombstones must remain TTL=0 (permanent). Add a code comment at the tombstone check site noting this is load-bearing for revocation correctness.

6. **Scope creep into KMS** — key versioning naturally pulls toward key registries, distribution protocols, and key escrow. Define the scope boundary in the phase plan before coding: "key_version is an integer in the envelope header and an array of KEM keypairs in the identity file. Nothing more." Mark key escrow, proxy re-encryption, and automatic re-encryption as explicitly out of scope.

7. **Group read-modify-write race** — concurrent admin sessions can produce lost-update conflicts on group membership blobs (latest-timestamp-wins). Accept the single-admin assumption for MVP, document it, and add a warning log when a group write timestamp is very close to the existing group's timestamp.

## Implications for Roadmap

Based on the dependency graph in FEATURES.md and the build order in ARCHITECTURE.md, the natural phase structure is four phases with clear dependencies.

### Phase 1: SDK Delegation Revocation

**Rationale:** Smallest scope, zero new format changes, uses only existing wire types (DelegationListRequest/Response types 51/52, delete_blob). Unblocks the admin revocation workflow with no architectural risk. Node enforcement is already proven and tested.
**Delivers:** `Directory.revoke_delegation()`, `Directory.list_delegates()`, revoke-then-re-delegate test coverage, PROTOCOL.md propagation window documentation.
**Addresses:** SDK delegation revocation helper (P1 must-have), revocation confirmation.
**Avoids:** Pitfall 7 (use existing tombstone mechanism, not a new revocation blob type), Pitfall 1 (document propagation window, do not add consensus), Pitfall 10 (document re-delegation semantics and add explicit test).

### Phase 2: KEM Key Versioning

**Rationale:** Most complex component but independent of Phase 1. Must precede Group Membership Revocation — Phase 3 depends on key rotation concepts being validated. The envelope header format decision (where key_version lives and as what type) is the highest-risk design decision in the milestone and must be locked before any rotation logic is written.
**Delivers:** UserEntry v2 format with `key_version: uint16 BE`, envelope header extended with `key_version: uint32 BE` as AEAD additional data, `Identity.rotate_kem()` with key ring, `Identity.save()`/`load()` with full ring persistence, modified `envelope_decrypt()` with key ring fallback on `NotARecipientError`, Directory cache handling UserEntry v2 (backward compat with v1 implied key_version=0).
**Uses:** Existing ML-KEM-1024 keygen, existing envelope stanza format (header extended, stanzas unchanged — `kem_pk_hash` already uniquely identifies the key used).
**Avoids:** Pitfall 5 (key_version in header as AEAD AD, not in payload), Pitfall 6 (atomic rotate + re-register in single SDK call), Pitfall 3 (old key retention mandatory), Pitfall 8 (KMS scope creep — define hard boundary before coding).

### Phase 3: Group Membership Revocation

**Rationale:** Straightforward once key versioning semantics from Phase 2 are established. GroupEntry v2 format mirrors the UserEntry v2 pattern. The actual member exclusion mechanism already works — `write_to_group()` resolves current members at call time and the per-blob DEK model means "removing a member from the group blob" is sufficient for forward exclusion.
**Delivers:** GroupEntry v2 format with `version_counter: uint32 BE`, `remove_member()` and `add_member()` incrementing version_counter, atomic `Directory.revoke_member(group_name, member)` orchestration (tombstone delegation + remove from group + rotate admin KEM key in one call), cache freshness check (`directory.refresh()`) before `write_to_group()` after removal, documentation that old data remains readable by removed members.
**Avoids:** Pitfall 4 (read-modify-write race — single-admin assumption documented, warning log on close-timestamp writes), Pitfall 3 (no re-encryption — document boundary clearly with correct SDK method naming: `remove_member()` not `revoke_access()`), Pitfall 9 (no group key rotation protocol — per-blob encryption model already handles it, `version_counter` is an epoch label only).

### Phase 4: Documentation and Protocol Update

**Rationale:** Always last — documents what was actually built, not what was planned. Ensures PROTOCOL.md reflects the exact wire format changes shipped in Phases 2 and 3, including UserEntry v2, envelope header extension, GroupEntry v2, revocation workflow, and propagation window bounds.
**Delivers:** PROTOCOL.md updates for all format changes, key rotation guide, group revocation guide, delegation management guide, inline code comments at tombstone check site explaining load-bearing nature.

### Phase Ordering Rationale

- Phase 1 first: zero dependencies, delivers immediate admin value, tombstone mechanism is proven — this phase is purely SDK surface addition.
- Phase 2 second: the envelope header format decision is the highest-risk design choice. Getting it right in Phase 2 does not affect Phase 1. If Phase 2 reveals unexpected complexity, Phases 3 and 4 can adjust.
- Phase 3 third: depends on Phase 2's key versioning concepts being validated. Group revocation composes from Phase 1 primitives (delegation tombstone) and Phase 2 patterns (key rotation).
- Phase 4 last: PROTOCOL.md documents shipped behavior, not intended behavior.

### Research Flags

Phases with well-documented patterns (skip research-phase):
- **Phase 1:** Zero uncertainty. Tombstone-based revocation is tested in production. DelegationList wire type exists. SDK method is pure plumbing against proven primitives.
- **Phase 3:** Straightforward once Phase 2 is validated. GroupEntry format change mirrors UserEntry v2 pattern. No novel design decisions.
- **Phase 4:** Documentation work. No research needed.

Phases needing planning-time validation:
- **Phase 2:** The envelope header format change requires three explicit decisions at plan time before coding: (a) `key_version` field width in envelope header (uint16 matching UserEntry v2 vs uint32 for future-proofing), (b) whether to bump envelope `version` byte from 0x01 to 0x02 (PITFALLS.md recommends bumping since pre-MVP), and (c) how `envelope_decrypt()` handles v1 envelopes post-change (imply key_version=0 for v1). These are design questions for the phase plan, not external research questions.

## Confidence Assessment

| Area | Confidence | Notes |
|------|------------|-------|
| Stack | HIGH | All findings verified against source code. No external research needed. Existing primitives cover 100% of requirements. |
| Features | HIGH | Feature boundaries verified against industry patterns (AWS KMS, GCP KMS, Signal, MLS RFC 9420). Anti-features list is concrete and grounded in PROJECT.md constraints. |
| Architecture | HIGH | Based on full codebase analysis of engine.cpp, storage.cpp, _envelope.py, _directory.py. Data flow diagrams verified against actual code paths. Component boundary table matches source file structure. |
| Pitfalls | HIGH | All pitfalls grounded in specific code locations or documented distributed systems failure modes. Recovery strategies are concrete. Tombstone ordering race analysis verified by re-reading ingest pipeline code. |

**Overall confidence:** HIGH

### Gaps to Address

- **Envelope version byte decision:** Research recommends bumping envelope version to 0x02 when adding `key_version` to the header (pre-MVP, no backward compat needed). However, the v1 envelope format has been shipping since v1.7.0 and is used in KVM integration tests and tutorial examples. Validate during Phase 2 planning whether to bump the version byte or handle key_version as a conditional field behind the version byte.

- **Identity file format for key ring:** The current `Identity.save()` writes individual files per keypair type (e.g., `.signing`, `.kem`). Persisting a key ring of multiple KEM keypairs needs a format decision (multiple numbered files vs. JSON manifest vs. binary bundle). Not a blocker but must be decided at the start of Phase 2 planning before implementation begins.

- **version_counter vs. key rotation distinction:** ARCHITECTURE.md adds `version_counter` to GroupEntry v2 for epoch tracking, but PITFALLS.md explicitly states that "group key rotation" is meaningless in chromatindb's per-blob model. Ensure Phase 3 does not conflate `version_counter` (a cosmetic epoch label for application use) with "key rotation" (which only applies to individual KEM keypairs). Document this distinction explicitly in Phase 3 plan.

## Sources

### Primary (HIGH confidence — source code verified)
- `db/engine/engine.cpp` — ingest pipeline, tombstone check at step 3.5, delegation check at step 2
- `db/storage/storage.cpp` — `has_valid_delegation()`, `delete_blob_data()`, delegation_map CRUD
- `sdk/python/chromatindb/_envelope.py` — envelope format, KEM-then-Wrap, stanza binary search by `kem_pk_hash`
- `sdk/python/chromatindb/_directory.py` — UserEntry/GroupEntry encode/decode, Directory cache, group read-modify-write
- `sdk/python/chromatindb/identity.py` — single KEM keypair per Identity, signing key handling
- `db/PROTOCOL.md` — delegation spec, envelope binary format, HKDF label registry
- `db/tests/engine/test_engine.cpp` lines 1090-1250 — delegation revocation tests, tombstone ordering

### Secondary (HIGH confidence — authoritative external sources)
- [MLS Protocol RFC 9420](https://datatracker.ietf.org/doc/rfc9420/) — group key rotation patterns, epoch semantics
- [MLS Architecture RFC 9750](https://www.rfc-editor.org/rfc/rfc9750.html) — member removal forward secrecy model
- [Google Cloud KMS Envelope Encryption](https://cloud.google.com/kms/docs/envelope-encryption) — key version management, old-version retention pattern
- [AWS KMS Key Rotation Best Practices](https://docs.aws.amazon.com/prescriptive-guidance/latest/aws-kms-best-practices/data-protection-key-rotation.html) — rotate-not-re-encrypt universal pattern
- [Signal Double Ratchet specification](https://signal.org/docs/specifications/doubleratchet/) — revocation induces ratchet for future messages only

### Tertiary (MEDIUM confidence — research and community)
- Protocol Labs Research #8 — decentralized access control CRDT concurrent update interpretation
- Hoop.dev access revocation blog — stale-permission window privilege escalation
- NuCypher KMS paper — key lifecycle management is a major subsystem (scope creep warning)
- Sahai, Seyalioglu, Waters (Crypto 2012) — revocation of past ciphertexts requires ciphertext-update, confirming old-data-readable constraint is fundamental

---
*Research completed: 2026-04-06*
*Ready for roadmap: yes*
