# Phase 77: Groups & Encrypted Client Helpers - Context

**Gathered:** 2026-04-02
**Status:** Ready for planning

<domain>
## Phase Boundary

Named group management (create/update/list groups in admin namespace) + convenience helpers on ChromatinClient (write_encrypted, read_encrypted, write_to_group) that combine envelope crypto with storage in one call. Pure SDK work -- no C++ node changes, no new wire types, zero new deps. Groups are blobs in the directory namespace using latest-timestamp-wins semantics. Encrypted helpers are methods on ChromatinClient.

</domain>

<decisions>
## Implementation Decisions

### Group Blob Format
- **D-01:** Magic bytes: `GRPE` (0x47525045) -- follows UENT/CENV naming convention
- **D-02:** Version byte: `0x01` for initial format
- **D-03:** Layout: `[GRPE:4][version:1][name_len:2 BE][name:N][member_count:2 BE][N x member_hash:32]`
- **D-04:** Members identified by SHA3-256(signing_pk) -- 32 bytes each. Compact; directory lookup resolves to full KEM pubkey at encrypt-time
- **D-05:** Group name is UTF-8 encoded, length-prefixed with uint16 big-endian (same as UserEntry display_name)

### Group Update Semantics
- **D-06:** Latest-timestamp-wins: multiple group blobs with the same name coexist. SDK picks the one with the highest timestamp as current version. No tombstones needed
- **D-07:** Read-modify-write for add_member/remove_member: read current group (latest-timestamp-wins), modify member list, write new group blob

### Group Administration
- **D-08:** Admin-only: only admin mode Directory can create/update/delete groups. Groups are written to admin's namespace
- **D-09:** add_member/remove_member accept Identity objects. SDK extracts SHA3-256(signing_pk) internally. Consistent with envelope_encrypt which takes Identity objects
- **D-10:** create_group takes group name + initial member list (list of Identity objects)

### Directory Cache Integration
- **D-11:** Groups share the same Directory cache. Directory._populate_cache() extended to decode both UENT and GRPE blobs in a single namespace scan
- **D-12:** Group index: `_groups: dict[str, GroupEntry]` keyed by group name, latest-timestamp-wins resolution during scan
- **D-13:** GroupEntry frozen dataclass: `GroupEntry(name: str, members: list[bytes], blob_hash: bytes, timestamp: int)`. Members list holds 32-byte pubkey hashes

### Helper Placement
- **D-14:** write_encrypted, read_encrypted, write_to_group are methods on ChromatinClient directly. Matches roadmap wording, single object API surface
- **D-15:** Directory passed explicitly per-call to write_to_group: `write_to_group(data, group_name, directory, ttl=...)`. No hidden state coupling

### write_encrypted API
- **D-16:** Signature: `write_encrypted(data: bytes, ttl: int, recipients: list[Identity] | None = None) -> WriteResult`. TTL is required (matches write_blob). recipients=None defaults to self-only encryption
- **D-17:** Returns WriteResult (blob_hash, seq_num, duplicate) -- consistent with write_blob
- **D-18:** Internally calls envelope_encrypt(data, recipients or [], self._identity) then write_blob(envelope_bytes, ttl)

### read_encrypted API
- **D-19:** Signature: `read_encrypted(namespace: bytes, blob_hash: bytes) -> bytes`. Returns decrypted plaintext bytes
- **D-20:** Calls read_blob, then envelope_decrypt on the data. Returns None if blob not found
- **D-21:** Raises NotARecipientError if blob exists but caller isn't a recipient (distinct from not-found). Matches Phase 75 envelope_decrypt behavior

### write_to_group API
- **D-22:** Signature: `write_to_group(data: bytes, group_name: str, directory: Directory, ttl: int) -> WriteResult`
- **D-23:** SDK resolves group name to member list via directory (GRP-04), looks up each member's KEM pubkey via directory cache, calls write_encrypted with resolved recipients
- **D-24:** Raises DirectoryError if group not found in directory

### Claude's Discretion
- GroupEntry placement (in _directory.py alongside DirectoryEntry, or in types.py)
- encode_group_entry/decode_group_entry internal helper structure
- Whether list_groups returns list[GroupEntry] or has a dedicated method vs directory.list_groups()
- Exact test case breakdown and assertion patterns
- Whether write_to_group resolves members via get_user_by_pubkey or a dedicated group resolution path

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Existing SDK modules (extend)
- `sdk/python/chromatindb/client.py` -- Add write_encrypted, read_encrypted, write_to_group methods
- `sdk/python/chromatindb/_directory.py` -- Extend Directory class with group methods (create_group, add_member, remove_member, list_groups, get_group). Extend _populate_cache to decode GRPE blobs. Add GroupEntry dataclass, encode/decode_group_entry
- `sdk/python/chromatindb/__init__.py` -- Re-export GroupEntry and any new public symbols

### Existing SDK modules (use directly)
- `sdk/python/chromatindb/_envelope.py` -- envelope_encrypt, envelope_decrypt (used by write_encrypted, read_encrypted)
- `sdk/python/chromatindb/identity.py` -- Identity class, from_public_keys(), namespace, kem_public_key
- `sdk/python/chromatindb/crypto.py` -- sha3_256 for member pubkey hashing
- `sdk/python/chromatindb/types.py` -- WriteResult, ReadResult (return types for helpers)
- `sdk/python/chromatindb/exceptions.py` -- NotARecipientError, DirectoryError (raised by helpers)

### Existing tests (extend)
- `sdk/python/tests/test_directory.py` -- Add group encode/decode/verify tests, Directory group methods tests
- `sdk/python/tests/test_client.py` -- Add write_encrypted/read_encrypted/write_to_group unit tests

### Prior phase context (decisions carried forward)
- `.planning/phases/75-identity-extension-envelope-crypto/75-CONTEXT.md` -- Envelope format, sender auto-inclusion (D-15), empty recipients = self-encrypt (D-16)
- `.planning/phases/76-directory-user-discovery/76-CONTEXT.md` -- Directory model, UserEntry format, cache/invalidation pattern, delegation flow

### Requirements
- `.planning/REQUIREMENTS.md` -- GRP-01 through GRP-04, CLI-01 through CLI-04

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `Directory._populate_cache()`: Scans namespace, decodes blobs, builds indexes -- extend for GRPE blobs
- `encode_user_entry()`/`decode_user_entry()`: Binary codec pattern to follow for group entries
- `envelope_encrypt()`/`envelope_decrypt()`: Called by write_encrypted/read_encrypted
- `Identity.from_public_keys()`: Reconstruct encrypt-capable identity from directory entries
- `crypto.sha3_256()`: Hash signing pubkey to member hash for group entries
- `DirectoryEntry` frozen dataclass: Pattern for GroupEntry

### Established Patterns
- Length-prefixed binary encoding with big-endian integers (struct.pack('>H', ...))
- Magic bytes + version byte header (UENT, CENV, DLGT)
- Frozen dataclasses in types.py / _directory.py for result types
- TYPE_CHECKING guard for circular imports (client.py <-> _directory.py)
- Drain-and-requeue notification pattern for cache invalidation

### Integration Points
- `client.py`: Import _envelope (envelope_encrypt, envelope_decrypt) and add 3 new methods
- `_directory.py`: Add GroupEntry, encode/decode_group_entry, extend Directory with group methods and _groups index
- `__init__.py`: Re-export GroupEntry
- Directory._populate_cache(): Extend to handle GRPE magic alongside UENT magic

</code_context>

<specifics>
## Specific Ideas

- Group blobs are just blobs with GRPE magic in the directory namespace -- they replicate, sync, and expire using standard node mechanisms
- Latest-timestamp-wins keeps group update simple (no tombstone dance) and mirrors how the directory cache already works with scanning
- Member hashes (32 bytes) instead of full pubkeys (2592 bytes) keeps group blobs compact -- a 50-member group is ~1.6 KiB vs ~130 KiB
- write_to_group resolves group to member KEM pubkeys at encrypt-time (GRP-04), meaning revoked members won't be included in future encryptions even without re-encrypting old data
- Self-encrypt via recipients=None is the simplest path for personal encrypted storage

</specifics>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope

</deferred>

---

*Phase: 77-groups-encrypted-client-helpers*
*Context gathered: 2026-04-02*
