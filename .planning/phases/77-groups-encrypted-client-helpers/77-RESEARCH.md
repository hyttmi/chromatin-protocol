# Phase 77: Groups & Encrypted Client Helpers - Research

**Researched:** 2026-04-02
**Domain:** Python SDK group management + encrypted client convenience methods
**Confidence:** HIGH

## Summary

Phase 77 extends the existing Python SDK with two distinct feature areas: (1) named group management in the admin-owned directory namespace, and (2) three convenience methods on ChromatinClient that combine envelope encryption with storage operations. Both areas build entirely on primitives delivered in Phases 75-76 (envelope crypto, directory, identity). No new dependencies, no C++ node changes, no new wire types.

The group subsystem extends `_directory.py` with a new GRPE binary format, a GroupEntry frozen dataclass, encode/decode functions, and five Directory methods (create_group, add_member, remove_member, list_groups, get_group). Groups are stored as blobs in the admin namespace with latest-timestamp-wins semantics. The encrypted client helpers add three methods to `client.py` (write_encrypted, read_encrypted, write_to_group) that compose envelope_encrypt/envelope_decrypt with write_blob/read_blob.

**Primary recommendation:** Split into two plans: (1) group management on Directory, (2) encrypted client helpers on ChromatinClient. Both are small enough for single-wave plans but are logically separable.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- D-01: GRPE magic bytes (0x47525045)
- D-02: Version byte 0x01
- D-03: Layout: [GRPE:4][version:1][name_len:2 BE][name:N][member_count:2 BE][N x member_hash:32]
- D-04: Members identified by SHA3-256(signing_pk) -- 32 bytes each
- D-05: Group name UTF-8, uint16 BE length prefix
- D-06: Latest-timestamp-wins for group updates
- D-07: Read-modify-write for add_member/remove_member
- D-08: Admin-only group create/update/delete
- D-09: add_member/remove_member accept Identity objects
- D-10: create_group takes group name + list[Identity]
- D-11: Groups share Directory cache, _populate_cache() extended for GRPE
- D-12: _groups: dict[str, GroupEntry] keyed by group name, latest-timestamp-wins
- D-13: GroupEntry frozen dataclass: GroupEntry(name, members, blob_hash, timestamp)
- D-14: write_encrypted, read_encrypted, write_to_group on ChromatinClient
- D-15: Directory passed explicitly to write_to_group
- D-16: write_encrypted(data, ttl, recipients=None) -> WriteResult; TTL required
- D-17: Returns WriteResult (consistent with write_blob)
- D-18: Internally calls envelope_encrypt then write_blob
- D-19: read_encrypted(namespace, blob_hash) -> bytes
- D-20: Calls read_blob then envelope_decrypt; returns None if blob not found
- D-21: Raises NotARecipientError if blob exists but caller not recipient
- D-22: write_to_group(data, group_name, directory, ttl) -> WriteResult
- D-23: Resolves group via directory, looks up KEM pubkeys, calls write_encrypted
- D-24: Raises DirectoryError if group not found

### Claude's Discretion
- GroupEntry placement (in _directory.py alongside DirectoryEntry, or in types.py)
- encode_group_entry/decode_group_entry internal helper structure
- Whether list_groups returns list[GroupEntry] or has a dedicated method vs directory.list_groups()
- Exact test case breakdown and assertion patterns
- Whether write_to_group resolves members via get_user_by_pubkey or a dedicated group resolution path

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| GRP-01 | Admin can create a named group with initial member list | D-08/D-10: admin-only create_group(name, members), GRPE binary format D-01..D-05 |
| GRP-02 | Admin can add or remove members from a group | D-07/D-09: read-modify-write pattern, accept Identity objects |
| GRP-03 | User can list groups and view group membership | D-11/D-12: shared cache, list_groups/get_group methods |
| GRP-04 | SDK resolves group name to member KEM pubkeys at encrypt-time | D-23: write_to_group resolves via directory cache, get_user_by_pubkey for each member hash |
| CLI-01 | write_encrypted(data, recipients) encrypts and stores | D-16/D-17/D-18: envelope_encrypt + write_blob composition |
| CLI-02 | read_encrypted(blob_hash) fetches, finds stanza, decrypts | D-19/D-20/D-21: read_blob + envelope_decrypt composition |
| CLI-03 | write_to_group(data, group_name) encrypts for all group members | D-22/D-23/D-24: group resolution + write_encrypted |
| CLI-04 | write_encrypted(data) with no recipients encrypts to self only | D-16: recipients=None defaults to self-only (envelope_encrypt handles sender auto-include) |
</phase_requirements>

## Standard Stack

No new libraries needed. All primitives already exist in the SDK.

### Core (Already Installed)
| Library | Version | Purpose | Status |
|---------|---------|---------|--------|
| liboqs-python | ~0.14.0 | ML-KEM-1024 encap for envelope encrypt | Already in deps |
| PyNaCl | ~1.5.0 | ChaCha20-Poly1305 AEAD | Already in deps |
| flatbuffers | ~25.12 | Wire format (not needed this phase) | Already in deps |

### Python Stdlib Used
| Module | Purpose |
|--------|---------|
| struct | BE integer packing for GRPE format |
| hashlib | SHA3-256 for member pubkey hashing |
| dataclasses | GroupEntry frozen dataclass |
| typing | TYPE_CHECKING guard |

**Installation:** None required -- zero new dependencies.

## Architecture Patterns

### Files Modified
```
sdk/python/chromatindb/
  _directory.py       # +GroupEntry, +encode/decode_group_entry, +5 Directory methods, extend _populate_cache
  client.py           # +write_encrypted, +read_encrypted, +write_to_group
  __init__.py         # +GroupEntry re-export
sdk/python/tests/
  test_directory.py   # +group codec tests, +group Directory method tests
  test_client.py      # +write_encrypted/read_encrypted/write_to_group tests
```

### Pattern 1: GRPE Binary Codec (follows UENT pattern)

**What:** Encode/decode group blobs using the same pattern as encode_user_entry/decode_user_entry.
**When to use:** All group blob read/write operations.

```python
# Follows exact pattern from _directory.py encode_user_entry
GROUPENTRY_MAGIC: bytes = b"GRPE"
GROUPENTRY_VERSION: int = 0x01

# Minimum size: magic(4) + ver(1) + name_len(2) + member_count(2) = 9 bytes
GROUPENTRY_MIN_SIZE: int = 9

def encode_group_entry(name: str, members: list[bytes]) -> bytes:
    """Encode a group blob: [GRPE:4][ver:1][name_len:2 BE][name:N][count:2 BE][N x hash:32]."""
    name_bytes = name.encode("utf-8")
    return (
        GROUPENTRY_MAGIC
        + struct.pack("B", GROUPENTRY_VERSION)
        + struct.pack(">H", len(name_bytes))
        + name_bytes
        + struct.pack(">H", len(members))
        + b"".join(members)
    )

def decode_group_entry(data: bytes) -> tuple[str, list[bytes]] | None:
    """Decode a GRPE blob. Returns (name, member_hashes) or None if invalid."""
    if len(data) < GROUPENTRY_MIN_SIZE:
        return None
    if data[:4] != GROUPENTRY_MAGIC:
        return None
    if data[4] != GROUPENTRY_VERSION:
        return None
    offset = 5
    name_len = struct.unpack(">H", data[offset:offset + 2])[0]
    offset += 2
    if offset + name_len > len(data):
        return None
    name = data[offset:offset + name_len].decode("utf-8")
    offset += name_len
    if offset + 2 > len(data):
        return None
    member_count = struct.unpack(">H", data[offset:offset + 2])[0]
    offset += 2
    if offset + member_count * 32 > len(data):
        return None
    members = []
    for _ in range(member_count):
        members.append(data[offset:offset + 32])
        offset += 32
    return name, members
```

### Pattern 2: Directory Cache Extension for Groups

**What:** Extend `_populate_cache()` to decode both UENT and GRPE blobs during namespace scan.
**When to use:** Directory lazy cache population.

```python
# In _populate_cache(), after reading each blob:
parsed_user = decode_user_entry(result.data)
if parsed_user is not None:
    # ... existing user entry handling ...
    continue

parsed_group = decode_group_entry(result.data)
if parsed_group is not None:
    name, member_hashes = parsed_group
    # Latest-timestamp-wins: only keep if newer
    existing = groups_by_name.get(name)
    if existing is None or result.timestamp > existing.timestamp:
        # result here is a ReadResult with .timestamp
        entry = GroupEntry(
            name=name,
            members=member_hashes,
            blob_hash=blob_ref.blob_hash,
            timestamp=result.timestamp,
        )
        groups_by_name[name] = entry
```

### Pattern 3: Client Helper Composition

**What:** write_encrypted/read_encrypted compose envelope crypto with client IO.
**When to use:** ChromatinClient convenience methods.

```python
# write_encrypted: envelope_encrypt + write_blob
async def write_encrypted(
    self, data: bytes, ttl: int, recipients: list[Identity] | None = None
) -> WriteResult:
    all_recipients = recipients if recipients is not None else []
    envelope = envelope_encrypt(data, all_recipients, self._identity)
    return await self.write_blob(envelope, ttl)

# read_encrypted: read_blob + envelope_decrypt
async def read_encrypted(
    self, namespace: bytes, blob_hash: bytes
) -> bytes | None:
    result = await self.read_blob(namespace, blob_hash)
    if result is None:
        return None
    return envelope_decrypt(result.data, self._identity)
```

### Pattern 4: Latest-Timestamp-Wins for Group Resolution

**What:** Multiple group blobs with the same name coexist. SDK picks the one with the highest timestamp.
**When to use:** Group cache build and read-modify-write operations.

```python
# When building group index during _populate_cache:
existing = groups_by_name.get(name)
if existing is None or blob_timestamp > existing.timestamp:
    groups_by_name[name] = new_entry

# Read-modify-write for add_member/remove_member:
# 1. Get current group (latest-timestamp-wins from cache)
# 2. Modify member list
# 3. Write new group blob (new timestamp will be > old)
```

### Pattern 5: Group Resolution for write_to_group

**What:** Resolve group name to list of Identity objects with KEM pubkeys.
**When to use:** write_to_group needs to convert member hashes to encrypt-capable Identities.

```python
# In write_to_group:
# 1. Get group from directory
group = await directory.get_group(group_name)
if group is None:
    raise DirectoryError(f"Group not found: {group_name}")

# 2. Resolve each member hash to Identity via directory cache
recipients = []
for member_hash in group.members:
    entry = await directory.get_user_by_pubkey(member_hash)
    if entry is not None:
        recipients.append(entry.identity)
    # Silently skip unresolved members (they may have been removed from directory)

# 3. Encrypt and write
return await self.write_encrypted(data, ttl, recipients)
```

### Recommended: GroupEntry Placement

**Recommendation:** Place GroupEntry in `_directory.py` alongside DirectoryEntry, not in types.py. Rationale: GroupEntry is tightly coupled to the directory subsystem (like DirectoryEntry), not a general-purpose protocol type like WriteResult. The encode/decode functions are in _directory.py, and the Directory class methods return GroupEntry. Keeping it co-located follows the existing pattern.

### Recommended: Group Methods on Directory

**Recommendation:** Add these methods to Directory class:
- `create_group(name, members)` -- admin-only
- `add_member(group_name, member)` -- admin-only, read-modify-write
- `remove_member(group_name, member)` -- admin-only, read-modify-write
- `list_groups()` -- returns list[GroupEntry] from cache
- `get_group(group_name)` -- returns GroupEntry | None from cache

### Recommended: Member Resolution Path

**Recommendation:** `write_to_group` resolves members via `directory.get_user_by_pubkey(member_hash)` for each member in the group. This reuses the existing directory cache (already has _by_pubkey_hash index). No dedicated group resolution path needed -- the existing index is O(1) per member.

### Anti-Patterns to Avoid
- **Do NOT add group resolution to client.py directly:** Keep group resolution in the Directory class or at the write_to_group call site. Client should not import _directory (already avoided via TYPE_CHECKING guard).
- **Do NOT use a shared mutable state for group tracking:** Groups are just blobs -- latest-timestamp-wins. No separate group state machine.
- **Do NOT add timestamp generation in Directory methods:** Write_blob in client.py already generates the timestamp. Directory.create_group calls client.write_blob which handles it.
- **Do NOT introduce circular import:** client.py uses `_envelope.py` functions directly (already imported at top). For write_to_group, the Directory is passed as a parameter, not imported.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Group member hashing | Custom hash function | `crypto.sha3_256(identity.public_key)` | Already exists, consistent with namespace derivation |
| Envelope encryption | Manual AEAD wrapping | `envelope_encrypt()` from _envelope.py | Already tested with 31 tests, handles KEM-then-Wrap |
| Blob writing with signing | Manual signing + encode | `client.write_blob()` | Already handles timestamp, signing, encoding |
| Cache invalidation | Custom pub/sub handler | Existing `_check_invalidation()` drain-and-requeue | Already tested, handles notification lifecycle |
| User lookup by pubkey hash | Custom index | `directory.get_user_by_pubkey()` | O(1) from existing _by_pubkey_hash index |

**Key insight:** This phase is pure composition. Every building block (envelope crypto, blob storage, directory cache, identity) already exists and is tested. The value is in wiring them together correctly with proper error handling.

## Common Pitfalls

### Pitfall 1: Latest-Timestamp-Wins Race Condition
**What goes wrong:** Two admins simultaneously modify a group. Both read the same "current" group, apply different changes, write back. One change is lost.
**Why it happens:** Read-modify-write without locking. This is inherent to the blob store design.
**How to avoid:** Accept this as a known limitation. Document that group management is designed for single-admin use. The last write wins. This matches the directory's existing behavior with user entries.
**Warning signs:** Multiple admin identities writing to the same directory namespace.

### Pitfall 2: Stale Group Members in write_to_group
**What goes wrong:** A member is removed from a group, but the directory cache still has the old group blob. write_to_group encrypts for the revoked member.
**Why it happens:** Cache invalidation via pub/sub is eventual, not immediate.
**How to avoid:** This is by-design ("forward-looking only" per requirements). The next cache refresh will pick up the new group blob. Document that write_to_group uses the cached group membership at call time.
**Warning signs:** Time gap between group modification and write_to_group call.

### Pitfall 3: Unresolvable Group Members
**What goes wrong:** A group member's hash is in the group blob, but their UserEntry is not in the directory (deleted, expired, or never registered).
**Why it happens:** Group membership is stored by signing_pk hash, not by full key. The user must be registered to resolve the hash to a KEM pubkey.
**How to avoid:** Silently skip unresolvable members during write_to_group resolution. The sender is always auto-included by envelope_encrypt. Log a warning for skipped members.
**Warning signs:** Group with member hashes that don't resolve to directory entries.

### Pitfall 4: Missing _identity on ChromatinClient for Encryption
**What goes wrong:** write_encrypted or read_encrypted called on a client that was constructed without an identity (or without KEM).
**Why it happens:** ChromatinClient.connect() takes identity as a parameter, but identity might lack KEM keypair for older identities.
**How to avoid:** write_encrypted should validate `self._identity.has_kem` before calling envelope_encrypt. envelope_encrypt already validates but a clear early error is better UX.
**Warning signs:** `ValueError: Sender must have KEM keypair` from deep in envelope_encrypt.

### Pitfall 5: Importing _envelope in client.py
**What goes wrong:** Circular import if done wrong. client.py already imports from _codec, _handshake, _transport.
**Why it happens:** _envelope.py imports from identity.py (TYPE_CHECKING only). client.py importing _envelope functions directly is safe -- no circular dependency exists because _envelope does not import client.
**How to avoid:** Direct top-level import of `envelope_encrypt` and `envelope_decrypt` in client.py. Already verified: _envelope.py has no client dependency.
**Warning signs:** ImportError at module load time.

### Pitfall 6: GroupEntry Timestamp Source
**What goes wrong:** Using the wrong timestamp for latest-timestamp-wins comparison.
**Why it happens:** ReadResult has a `.timestamp` field (from the blob metadata). This is the blob's creation timestamp, set by write_blob. GroupEntry needs this timestamp to determine which group version is current.
**How to avoid:** Pass `result.timestamp` from ReadResult into GroupEntry during cache population. Do NOT use a separate timestamp field in the GRPE binary format -- the blob-level timestamp is canonical.
**Warning signs:** GroupEntry.timestamp == 0 or incorrect values.

### Pitfall 7: Empty Group Creation
**What goes wrong:** create_group called with an empty member list. Is this valid?
**How to avoid:** Per D-10, create_group takes an initial member list. An empty list should be valid (admin may want to create a group and add members later). The GRPE format supports member_count=0. encode_group_entry should handle this case. However, write_to_group with an empty group would encrypt to self only (empty recipients -> sender auto-include), which is valid.

## Code Examples

### GroupEntry Frozen Dataclass (D-13)
```python
# In _directory.py, alongside DirectoryEntry
@dataclass(frozen=True)
class GroupEntry:
    """A group entry from the directory cache.

    Attributes:
        name: Group display name.
        members: List of 32-byte SHA3-256(signing_pk) hashes.
        blob_hash: 32-byte hash of the blob containing this entry.
        timestamp: Blob timestamp (for latest-timestamp-wins resolution).
    """
    name: str
    members: list[bytes]
    blob_hash: bytes
    timestamp: int
```

### write_encrypted on ChromatinClient (D-16..D-18)
```python
async def write_encrypted(
    self,
    data: bytes,
    ttl: int,
    recipients: list[Identity] | None = None,
) -> WriteResult:
    """Encrypt data and write as a blob.

    Args:
        data: Plaintext bytes to encrypt.
        ttl: Time-to-live in seconds.
        recipients: Recipient identities (each must have KEM pubkey).
            None or omitted: encrypt to self only (CLI-04).

    Returns:
        WriteResult with blob_hash, seq_num, duplicate.

    Raises:
        ValueError: If sender or any recipient lacks KEM keypair.
    """
    all_recipients = recipients if recipients is not None else []
    envelope = envelope_encrypt(data, all_recipients, self._identity)
    return await self.write_blob(envelope, ttl)
```

### read_encrypted on ChromatinClient (D-19..D-21)
```python
async def read_encrypted(
    self, namespace: bytes, blob_hash: bytes
) -> bytes | None:
    """Fetch and decrypt an encrypted blob.

    Args:
        namespace: 32-byte namespace identifier.
        blob_hash: 32-byte blob hash.

    Returns:
        Decrypted plaintext bytes, or None if blob not found.

    Raises:
        NotARecipientError: If blob exists but caller is not a recipient.
        MalformedEnvelopeError: If blob data is not a valid envelope.
        DecryptionError: If AEAD authentication fails.
    """
    result = await self.read_blob(namespace, blob_hash)
    if result is None:
        return None
    return envelope_decrypt(result.data, self._identity)
```

### write_to_group on ChromatinClient (D-22..D-24)
```python
async def write_to_group(
    self,
    data: bytes,
    group_name: str,
    directory: Directory,
    ttl: int,
) -> WriteResult:
    """Encrypt data for all group members and write as a blob.

    Resolves group membership at call time via directory cache (GRP-04).

    Args:
        data: Plaintext bytes to encrypt.
        group_name: Name of the group in the directory.
        directory: Directory instance for group and member resolution.
        ttl: Time-to-live in seconds.

    Returns:
        WriteResult with blob_hash, seq_num, duplicate.

    Raises:
        DirectoryError: If group not found in directory.
        ValueError: If any resolved member lacks KEM pubkey.
    """
    group = await directory.get_group(group_name)
    if group is None:
        raise DirectoryError(f"Group not found: {group_name}")
    recipients = []
    for member_hash in group.members:
        entry = await directory.get_user_by_pubkey(member_hash)
        if entry is not None:
            recipients.append(entry.identity)
    return await self.write_encrypted(data, ttl, recipients)
```

### create_group on Directory (GRP-01)
```python
async def create_group(
    self, name: str, members: list[Identity]
) -> WriteResult:
    """Create a named group with initial members.

    Args:
        name: Group display name.
        members: Initial member identities.

    Returns:
        WriteResult from writing the group blob.

    Raises:
        DirectoryError: If not in admin mode.
        ValueError: If name exceeds 256 UTF-8 bytes.
    """
    if not self._is_admin:
        raise DirectoryError("only admin can create groups")
    member_hashes = [sha3_256(m.public_key) for m in members]
    group_data = encode_group_entry(name, member_hashes)
    result = await self._client.write_blob(group_data, ttl=0)
    self._dirty = True
    return result
```

### add_member / remove_member on Directory (GRP-02)
```python
async def add_member(
    self, group_name: str, member: Identity
) -> WriteResult:
    """Add a member to an existing group (read-modify-write)."""
    if not self._is_admin:
        raise DirectoryError("only admin can modify groups")
    group = await self.get_group(group_name)
    if group is None:
        raise DirectoryError(f"Group not found: {group_name}")
    member_hash = sha3_256(member.public_key)
    if member_hash in group.members:
        raise DirectoryError(f"Member already in group: {group_name}")
    new_members = list(group.members) + [member_hash]
    group_data = encode_group_entry(group_name, new_members)
    result = await self._client.write_blob(group_data, ttl=0)
    self._dirty = True
    return result

async def remove_member(
    self, group_name: str, member: Identity
) -> WriteResult:
    """Remove a member from a group (read-modify-write)."""
    if not self._is_admin:
        raise DirectoryError("only admin can modify groups")
    group = await self.get_group(group_name)
    if group is None:
        raise DirectoryError(f"Group not found: {group_name}")
    member_hash = sha3_256(member.public_key)
    if member_hash not in group.members:
        raise DirectoryError(f"Member not in group: {group_name}")
    new_members = [m for m in group.members if m != member_hash]
    group_data = encode_group_entry(group_name, new_members)
    result = await self._client.write_blob(group_data, ttl=0)
    self._dirty = True
    return result
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Direct envelope_encrypt + write_blob calls | write_encrypted one-liner | Phase 77 | Simpler user API |
| Manual group tracking outside SDK | Directory-managed groups with cache | Phase 77 | Integrated group management |

**No deprecations.** This phase adds new APIs without changing existing ones.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | pytest 9.0.2 |
| Config file | sdk/python/pyproject.toml [tool.pytest.ini_options] |
| Quick run command | `cd sdk/python && pytest tests/test_directory.py tests/test_client.py -x -q` |
| Full suite command | `cd sdk/python && pytest tests/ -x -q` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| GRP-01 | Admin creates group with members | unit | `pytest tests/test_directory.py -x -q -k "create_group"` | Extend existing |
| GRP-02 | Admin add/remove members | unit | `pytest tests/test_directory.py -x -q -k "add_member or remove_member"` | Extend existing |
| GRP-03 | List groups, view membership | unit | `pytest tests/test_directory.py -x -q -k "list_groups or get_group"` | Extend existing |
| GRP-04 | Resolve group to KEM pubkeys | unit | `pytest tests/test_client.py -x -q -k "write_to_group"` | Extend existing |
| CLI-01 | write_encrypted with recipients | unit | `pytest tests/test_client.py -x -q -k "write_encrypted"` | Extend existing |
| CLI-02 | read_encrypted decrypts blob | unit | `pytest tests/test_client.py -x -q -k "read_encrypted"` | Extend existing |
| CLI-03 | write_to_group encrypts for group | unit | `pytest tests/test_client.py -x -q -k "write_to_group"` | Extend existing |
| CLI-04 | write_encrypted self-only | unit | `pytest tests/test_client.py -x -q -k "write_encrypted_self"` | Extend existing |

### Sampling Rate
- **Per task commit:** `cd sdk/python && pytest tests/test_directory.py tests/test_client.py -x -q`
- **Per wave merge:** `cd sdk/python && pytest tests/ -x -q`
- **Phase gate:** Full suite green before /gsd:verify-work

### Wave 0 Gaps
None -- existing test infrastructure covers all phase requirements. Test files exist; tests will be appended. pytest config, conftest.py fixtures, and asyncio_mode="auto" are all in place.

## Open Questions

1. **Duplicate group name handling**
   - What we know: D-06 says latest-timestamp-wins, so writing a second group with the same name creates a new "version." There is no unique constraint.
   - What's unclear: Should create_group check if a group with that name already exists and raise DirectoryError? Or silently overwrite?
   - Recommendation: create_group should NOT check -- it is a "create or overwrite" operation consistent with how the blob store works. If the caller wants to check, they can call get_group first. Keep the interface simple.

2. **GroupEntry member list immutability**
   - What we know: GroupEntry is a frozen dataclass. Members is `list[bytes]`.
   - What's unclear: Frozen dataclass does not deep-freeze the members list. Caller could mutate the list after construction.
   - Recommendation: Store members as `tuple[bytes, ...]` internally for true immutability, or accept that this is Python and document the contract. The latter matches DirectoryEntry.identity which is also mutable in theory. Follow the existing pattern -- use `list[bytes]` per D-13.

## Sources

### Primary (HIGH confidence)
- `sdk/python/chromatindb/_directory.py` -- existing Directory class, cache pattern, UENT codec (direct code inspection)
- `sdk/python/chromatindb/client.py` -- existing ChromatinClient API surface (direct code inspection)
- `sdk/python/chromatindb/_envelope.py` -- envelope_encrypt/envelope_decrypt signatures and behavior (direct code inspection)
- `sdk/python/chromatindb/identity.py` -- Identity class, from_public_keys, has_kem, kem_public_key (direct code inspection)
- `sdk/python/chromatindb/types.py` -- WriteResult, ReadResult definitions (direct code inspection)
- `sdk/python/chromatindb/exceptions.py` -- NotARecipientError, DirectoryError hierarchy (direct code inspection)
- `sdk/python/chromatindb/crypto.py` -- sha3_256 function (direct code inspection)
- `sdk/python/tests/test_directory.py` -- existing test patterns, mock helpers, 46 tests (direct code inspection)
- `sdk/python/tests/conftest.py` -- identity fixture, asyncio_mode=auto config (direct code inspection)

### Secondary (MEDIUM confidence)
- `.planning/phases/75-identity-extension-envelope-crypto/75-CONTEXT.md` -- envelope format decisions (project documentation)
- `.planning/phases/76-directory-user-discovery/76-CONTEXT.md` -- directory model decisions (project documentation)

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- zero new deps, all primitives verified in source
- Architecture: HIGH -- extends existing patterns with direct code inspection
- Pitfalls: HIGH -- derived from actual code structure and known async/cache behaviors

**Research date:** 2026-04-02
**Valid until:** 2026-05-02 (stable -- no external dependencies, all internal code)
