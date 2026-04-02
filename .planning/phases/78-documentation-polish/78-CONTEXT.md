# Phase 78: Documentation & Polish - Context

**Gathered:** 2026-04-02
**Status:** Ready for planning

<domain>
## Phase Boundary

Document all v1.7.0 client-side encryption features across three existing doc files: PROTOCOL.md (envelope binary format spec + HKDF label registry), SDK README (encryption API section), and getting-started tutorial (encryption workflow example). Pure documentation -- no code changes, no new features.

</domain>

<decisions>
## Implementation Decisions

### PROTOCOL.md Envelope Spec (DOC-01)
- **D-01:** Byte-level format tables matching existing PROTOCOL.md style (offset/size/field/description columns)
- **D-02:** New section "Client-Side Envelope Encryption" after "SDK Client Notes" or as a peer section to existing transport/message docs
- **D-03:** Document the complete envelope binary format: [magic:4][version:1][suite:1][recipient_count:2 BE][data_nonce:12][N x (kem_pk_hash:32 + kem_ct:1568 + wrapped_dek:48)][ciphertext+tag]
- **D-04:** HKDF Label Registry section listing all four domain labels: two transport ("chromatindb-session-keys-v1" send/recv info strings), one DARE ("chromatindb-dare-v1"), one envelope KEK ("chromatindb-envelope-kek-v1")
- **D-05:** Document the KEM-then-Wrap pattern: ML-KEM-1024 encapsulation produces shared secret, HKDF derives KEK, KEK wraps random DEK via ChaCha20-Poly1305
- **D-06:** Document AEAD parameters for data encryption: random 12-byte nonce (NOT counter-based), ChaCha20-Poly1305, full header as AD

### SDK README Encryption Section (DOC-02)
- **D-07:** Add "Encryption" section to API Overview with sub-tables: Encryption Operations, Directory & Groups
- **D-08:** Table format matching existing method tables (Method | Description | Returns columns)
- **D-09:** Cover write_encrypted, read_encrypted, write_to_group, Directory (delegate, register, list_users, get_user, create_group, add_member, remove_member, list_groups, get_group), GroupEntry re-export
- **D-10:** Quick Start example updated to show encrypted write/read alongside existing plaintext example

### Getting Started Tutorial (DOC-03)
- **D-11:** Extend existing tutorial (sdk/python/docs/getting-started.md) with new sections at the end, not a separate file
- **D-12:** Step-by-step narrative: Identity with KEM → Directory setup (admin) → User registration → Group creation → write_encrypted → read_encrypted → write_to_group
- **D-13:** Show both self-encrypt (recipients=None) and multi-recipient patterns
- **D-14:** Match existing tutorial style: explanation paragraph → code block → brief notes

### Claude's Discretion
- Exact placement of envelope spec section within PROTOCOL.md
- Whether to add a "Concepts" subsection explaining envelope encryption at a high level before the byte-level spec
- Exact ordering of encryption methods in README tables
- Whether tutorial encryption section includes error handling examples (NotARecipientError)

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Documents to update
- `db/PROTOCOL.md` -- Add envelope format spec section and HKDF label registry (DOC-01)
- `sdk/python/README.md` -- Add encryption API section (DOC-02)
- `sdk/python/docs/getting-started.md` -- Add encryption workflow tutorial (DOC-03)

### Source code (read for accurate documentation)
- `sdk/python/chromatindb/_envelope.py` -- envelope_encrypt, envelope_decrypt, parse_envelope_header, ENVELOPE_MAGIC, SUITE_CHACHA20_MLKEM1024
- `sdk/python/chromatindb/client.py` -- write_encrypted, read_encrypted, write_to_group signatures and docstrings
- `sdk/python/chromatindb/_directory.py` -- Directory class, GroupEntry, encode/decode functions, all group methods
- `sdk/python/chromatindb/identity.py` -- Identity.generate(), save(), load(), from_public_keys(), has_kem, kem_public_key
- `sdk/python/chromatindb/crypto.py` -- HKDF domain labels, sha3_256

### Prior phase context (format decisions)
- `.planning/phases/75-identity-extension-envelope-crypto/75-CONTEXT.md` -- Envelope format locked in Phase 75
- `.planning/phases/76-directory-user-discovery/76-CONTEXT.md` -- Directory model, UserEntry format, delegation
- `.planning/phases/77-groups-encrypted-client-helpers/77-CONTEXT.md` -- GRPE format, group API, encrypted helpers

### Requirements
- `.planning/REQUIREMENTS.md` -- DOC-01, DOC-02, DOC-03

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- PROTOCOL.md byte-level format tables: established pattern with Offset/Size/Field/Description columns
- README.md API Overview tables: Method/Description/Returns columns, grouped by category
- getting-started.md tutorial: paragraph → code block → notes pattern, sequential workflow

### Established Patterns
- PROTOCOL.md uses ASCII art for handshake flows and `|` delimited byte format tables
- README method tables are organized: Data Operations, Query Operations, Pub/Sub, Utility
- Tutorial sections follow: Prerequisites → Identity → Connect → Operations → Advanced

### Integration Points
- PROTOCOL.md: new section peer to existing "SDK Client Notes" or before it
- README.md: new "Encryption" section after "Pub/Sub" or before "Utility"
- getting-started.md: new encryption sections after existing delete/query content

</code_context>

<specifics>
## Specific Ideas

- HKDF label registry is a single reference table that eliminates ambiguity about which HKDF context string goes where -- critical for cross-language SDK implementers
- Envelope format spec must document per-recipient overhead (1648 bytes: 32 hash + 1568 KEM ct + 48 wrapped DEK) so implementers can size-estimate envelopes
- Tutorial should end with a "what's next" that positions the encryption features as zero-knowledge storage
- Content-addressed dedup breaks on encrypted data -- worth a one-liner note in PROTOCOL.md so implementers aren't surprised

</specifics>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope

</deferred>

---

*Phase: 78-documentation-polish*
*Context gathered: 2026-04-02*
