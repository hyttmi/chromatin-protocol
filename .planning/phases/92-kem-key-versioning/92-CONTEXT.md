# Phase 92: KEM Key Versioning - Context

**Gathered:** 2026-04-07
**Status:** Ready for planning

<domain>
## Phase Boundary

Users can rotate their ML-KEM-1024 encryption keypair so a compromised key cannot decrypt future data. Old data remains readable via key ring fallback. All SDK-only Python changes.

</domain>

<decisions>
## Implementation Decisions

### Key Ring Persistence
- **D-01:** Numbered files on disk: `kem.0.sec`/`kem.0.pub` (oldest), `kem.1.sec`/`kem.1.pub`, etc. `kem.sec`/`kem.pub` are always copies of the latest version.
- **D-02:** `Identity.load()` globs `kem.*.sec` to discover all historical keys. Builds key ring ordered by version number.
- **D-03:** Lazy migration: pre-rotation identities (only `kem.sec`/`kem.pub`, no numbered files) are treated as version 0 in memory. No files created on load. First `rotate_kem()` writes `kem.0.*` (copy of original) + `kem.1.*` (new) + updates `kem.sec`/`kem.pub`.

### UserEntry Format
- **D-04:** No backward compatibility. Single UserEntry format with `key_version` field. No v1/v2 distinction, no version byte gymnastics, no fallback parsing.
- **D-05:** Format: `[magic:4][version:1][signing_pk:2592][kem_pk:1568][name_len:2 BE][name:N][key_version:4 BE][kem_sig:variable]`. `kem_sig` signs `(kem_pk || key_version_be)`.
- **D-06:** `register()` always writes this format. Fresh identities use `key_version=0`. After rotation, `key_version` increments.
- **D-07:** `resolve_recipient()` returns the entry with the highest `key_version` for a given signing key.

### Envelope Format
- **D-08:** No envelope format changes. Stanzas already carry `kem_pk_hash` which identifies which key encrypted the stanza. Decryptor builds a `hash -> secret_key` map from the ring and does O(1) lookup.

### Rotation API
- **D-09:** `Identity.rotate_kem()` is offline-only. Generates new KEM keypair, moves old to ring, bumps `key_version`, saves numbered files. No network dependency — consistent with Identity being network-free.
- **D-10:** Caller publishes the updated key via `directory.register()` after rotation. Two-step process: rotate locally, then publish.

### No Backward Compatibility (project-wide rule)
- **D-11:** Pre-MVP: no backward compat anywhere. Old UserEntry format blobs are simply not parsed. Old identities without key ring files are handled lazily (D-03) but no migration tooling or dual-format support.

### Claude's Discretion
- Key ring internal data structure (list of tuples, dict by version, etc.)
- Exact `rotate_kem()` implementation details (file write ordering, atomicity)
- Test fixture design and mock structure
- Error message wording

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Identity & Key Management
- `sdk/python/chromatindb/identity.py` -- Current Identity class with KEM keypair (generate, save, load, from_public_keys). Key ring additions go here.
- `sdk/python/chromatindb/_directory.py` -- UserEntry format (make_user_entry, parse_user_entry, verify_user_entry), register(), resolve_recipient(). Format and resolution changes go here.

### Envelope Encryption
- `sdk/python/chromatindb/_envelope.py` -- envelope_encrypt() uses recipient.kem_public_key (line ~136), envelope_decrypt() matches sha3_256(identity.kem_public_key) against stanza kem_pk_hash (line ~254). Decrypt needs key ring iteration.

### Protocol Documentation
- `db/PROTOCOL.md` -- Envelope format specification, UserEntry format (will need update in Phase 94)

### Requirements
- `.planning/REQUIREMENTS.md` -- KEY-01, KEY-02, KEY-03

### Tests
- `sdk/python/tests/test_directory.py` -- Existing directory tests. New key versioning tests go here.
- `sdk/python/tests/test_envelope.py` -- Existing envelope encryption tests. Key ring decrypt tests go here.
- `sdk/python/tests/test_identity.py` -- Existing identity tests. Key ring persistence tests go here.

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `Identity.generate()` -- Creates signing + KEM keypairs. rotate_kem() follows same pattern for KEM-only.
- `Identity.save(path)` / `Identity.load(path)` -- Flat file persistence. Extend for numbered KEM files.
- `make_user_entry(identity)` -- Builds UserEntry blob. Add key_version field.
- `parse_user_entry(data)` -- Parses UserEntry. Add key_version extraction.
- `verify_user_entry(signing_pk, kem_pk, kem_sig)` -- Verify sig. Update to include key_version in signed data.
- `envelope_decrypt(data, identity)` -- Single-key decrypt. Extend to try all ring keys via pk_hash map.

### Established Patterns
- Identity has no network dependency (no client reference)
- Directory methods use `self._client` for all wire operations
- UserEntry magic bytes: `b"USER"` (4 bytes)
- KEM keys: ML-KEM-1024 (1568 byte public, 3168 byte secret)
- `sha3_256()` used for all hashing (pk_hash, namespace, etc.)

### Integration Points
- `identity.py` -- rotate_kem(), key_ring property, save/load changes
- `_directory.py` -- UserEntry format, register(), resolve_recipient() highest-version logic
- `_envelope.py` -- envelope_decrypt() key ring fallback
- `__init__.py` -- Export any new types if needed

</code_context>

<specifics>
## Specific Ideas

- Key ring as list of `(version, kem_public_key, kem_secret_key_or_none)` tuples, ordered by version
- `envelope_decrypt` builds `{sha3_256(pk): (version, sk)}` dict from ring, O(1) stanza matching
- `resolve_recipient()` filters UserEntry blobs by signing key, picks max `key_version`
- `Identity.key_version` property returns current (highest) version number

</specifics>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope

</deferred>

---

*Phase: 92-kem-key-versioning*
*Context gathered: 2026-04-07*
