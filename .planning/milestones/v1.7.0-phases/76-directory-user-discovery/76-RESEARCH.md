# Phase 76: Directory & User Discovery - Research

**Researched:** 2026-04-01
**Domain:** SDK-side directory namespace management, user self-registration, pubkey discovery, cache with pub/sub invalidation
**Confidence:** HIGH

## Summary

Phase 76 is pure Python SDK work -- no C++ node changes, no new wire types, no new dependencies. A new `_directory.py` module wraps existing `ChromatinClient` primitives (`write_blob`, `read_blob`, `list_blobs`, `subscribe`) into a higher-level directory API. Users can publish their ML-KEM-1024 encryption public keys via a self-registration flow and discover other users' pubkeys for envelope encryption.

All building blocks already exist: `Identity.from_public_keys()` (Phase 75) for reconstructing encrypt-capable identities from directory entries, `write_blob()` for storing UserEntry blobs and delegation blobs, `list_blobs()` + `read_blob()` for enumeration, and `subscribe()` for cache invalidation. The UserEntry binary format follows established `_codec.py` patterns (struct.pack, big-endian, length-prefixed fields, 4-byte magic prefix).

The primary complexity is in the cache invalidation mechanism -- the existing Transport has a single notification queue consumed by `client.notifications()`. The Directory needs to observe directory namespace notifications without stealing them from user code. The recommended approach is a background `asyncio.Task` that drains the notification queue, sets a dirty flag for directory notifications, and re-enqueues all notifications for user consumption.

**Primary recommendation:** Build `_directory.py` as a self-contained module with `Directory` class, `DirectoryEntry` frozen dataclass, `DirectoryError` exception, and `encode_user_entry`/`decode_user_entry` codec functions. Use a background task for notification-driven cache invalidation with a dirty flag + full rebuild on next access.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- D-01: A directory is a configuration object (Directory class) wrapping a ChromatinClient and an admin namespace. The "directory" IS the admin's namespace
- D-02: Admin creates a directory by constructing `Directory(client, admin_identity)` where `admin_identity.namespace` defines the directory namespace
- D-03: Non-admin users interact via `Directory(client, user_identity, directory_namespace=admin_namespace)` to read and self-register
- D-04: New module: `sdk/python/chromatindb/_directory.py` -- separate from client.py
- D-05: UserEntry is raw binary with length-prefixed fields, consistent with _codec.py patterns
- D-06: UserEntry layout: `[magic:4][version:1][signing_pk:2592][kem_pk:1568][name_len:2 BE][display_name:N][kem_sig:4627]`
- D-07: Magic bytes: `UENT` (0x55454E54). Version: 0x01
- D-08: `kem_sig` is ML-DSA-87 signature of the KEM public key bytes, signed by the registering user's signing key
- D-09: Display name is UTF-8, max 256 bytes, length-prefixed with uint16 big-endian
- D-10: UserEntry is written as a regular signed blob to the directory namespace
- D-11: Two-step registration: (1) Admin delegates, (2) User self-registers
- D-12: Admin delegation uses existing write_blob() with delegation encoding
- D-13: `Directory.register(display_name)` method: builds UserEntry, calls client.write_blob(). Returns blob hash
- D-14: Registration is idempotent -- writing same UserEntry returns duplicate=True
- D-15: `Directory.list_users()` returns all UserEntry entries. Uses list_blobs() + read_blob()
- D-16: `Directory.get_user(display_name)` returns single UserEntry by name. None if not found
- D-17: `Directory.get_user_by_pubkey(pubkey_hash)` returns UserEntry by SHA3-256(signing_pk). None if not found
- D-18: Lookup methods return Identity object via Identity.from_public_keys() for immediate encryption use
- D-19: Return type: `DirectoryEntry` frozen dataclass with `identity: Identity`, `display_name: str`, `blob_hash: bytes`
- D-20: In-memory cache: `dict[bytes, DirectoryEntry]` keyed by blob_hash
- D-21: Cache populated lazily on first list_users() or get_user() call. Full namespace scan
- D-22: Subscribe to directory namespace on cache populate. On notification, clear entire cache. Next access triggers full refresh
- D-23: `Directory.refresh()` method for explicit cache invalidation
- D-24: Secondary indexes: `_by_name: dict[str, DirectoryEntry]` and `_by_pubkey_hash: dict[bytes, DirectoryEntry]` for O(1) lookup
- D-25: Verify each UserEntry's kem_sig on cache populate. Skip entries that fail (log warning)
- D-26: register() verifies identity has KEM keypair (has_kem must be True). Raise ValueError if not
- D-27: get_user/get_user_by_pubkey return only verified entries from cache
- D-28: New exception: `DirectoryError(ChromatinError)`
- D-29: Registration without prior delegation fails at node level -- SDK raises DirectoryError

### Claude's Discretion
- Internal cache data structures and index rebuild strategy
- Whether Directory uses __aenter__/__aexit__ or just regular class
- Exact test case breakdown and assertion patterns
- Whether list_users() returns list or async iterator
- Notification callback mechanism (background task vs on-demand check)

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| DIR-01 | Admin can create an org directory backed by a namespace they own | Directory constructor with admin_identity maps to D-01/D-02. Delegation mechanism documented in Architecture Patterns. |
| DIR-02 | User can self-register by publishing a signed UserEntry blob to the directory via delegation | Two-step flow (D-11/D-12/D-13): admin writes delegation blob, user writes UserEntry. Binary format in D-06. Delegation wire format verified against C++ PROTOCOL.md. |
| DIR-03 | UserEntry contains signing pubkey, KEM pubkey, display name, and ML-DSA-87 signature over KEM pubkey | UserEntry format (D-06): `[UENT:4][ver:1][signing_pk:2592][kem_pk:1568][name_len:2 BE][name:N][kem_sig:4627]`. Total: 8794 + name_len bytes. kem_sig prevents MITM key substitution. |
| DIR-04 | User can list all registered users in a directory | list_users() (D-15) uses existing list_blobs() pagination + read_blob() + UserEntry decode. Cache populated lazily (D-21). |
| DIR-05 | User can fetch another user's KEM pubkey by display name or pubkey hash | get_user(name) and get_user_by_pubkey(hash) (D-16/D-17) use O(1) secondary indexes (D-24). Returns DirectoryEntry with Identity (D-18/D-19). |
| DIR-06 | SDK caches directory entries in memory and invalidates via pub/sub notifications | In-memory cache (D-20) with subscribe on populate (D-22). Notification triggers full cache clear. Background task or lazy drain pattern for notification consumption. |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| Python | >=3.10 | Runtime | Already pinned in pyproject.toml |
| liboqs-python | ~=0.14.0 | ML-DSA-87 verify (kem_sig validation) | Already a dependency, used for Identity.verify() |
| asyncio | stdlib | Background task for cache invalidation | Already used throughout SDK for async IO |
| struct | stdlib | Binary encode/decode for UserEntry | Already used in _codec.py for all wire formats |
| hashlib | stdlib | SHA3-256 for pubkey hashing | Already used via crypto.sha3_256 |
| logging | stdlib | Warning log for invalid UserEntry skips | Standard Python logging |

### Supporting
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| pytest | latest | Unit tests | All directory tests |
| pytest-asyncio | latest | Async test support | asyncio_mode = "auto" already configured |

### Alternatives Considered
None -- this phase uses zero new dependencies. Everything needed is already in the SDK.

## Architecture Patterns

### Recommended Module Layout
```
sdk/python/chromatindb/
  _directory.py          NEW: Directory class, encode/decode UserEntry, DirectoryEntry
  exceptions.py          MODIFY: add DirectoryError
  __init__.py            MODIFY: re-export Directory, DirectoryEntry, DirectoryError
```

### Pattern 1: Magic Prefix Dispatch
**What:** 4-byte magic prefix identifies blob content type.
**When:** Any time structured data is stored as a blob.
**Existing precedent:** TOMBSTONE_MAGIC (0xDEADBEEF), DELEGATION_MAGIC (0xDE1E6A7E), ENVELOPE_MAGIC (CENV).
**For this phase:** UserEntry uses `UENT` (0x55454E54) per D-07.

```python
# Source: CONTEXT.md D-07
USERENTRY_MAGIC = b"UENT"  # 0x55454E54
USERENTRY_VERSION = 0x01
```

### Pattern 2: Frozen Dataclass for Result Types
**What:** All public result types are `@dataclass(frozen=True)`.
**When:** Any data returned to callers.
**Existing precedent:** All 18 types in types.py (WriteResult, ReadResult, BlobRef, etc.)

```python
# Source: types.py pattern, CONTEXT.md D-19
@dataclass(frozen=True)
class DirectoryEntry:
    identity: Identity
    display_name: str
    blob_hash: bytes
```

### Pattern 3: Binary Encode/Decode with struct.pack
**What:** Length-prefixed binary fields, big-endian integers, fixed-size keys.
**When:** Serializing/deserializing UserEntry blobs.
**Existing precedent:** Every function in _codec.py.

```python
# Source: _codec.py patterns, CONTEXT.md D-06
def encode_user_entry(
    signing_pk: bytes,
    kem_pk: bytes,
    display_name: str,
    kem_sig: bytes,
) -> bytes:
    name_bytes = display_name.encode("utf-8")
    return (
        USERENTRY_MAGIC
        + struct.pack("B", USERENTRY_VERSION)
        + signing_pk          # 2592 bytes
        + kem_pk              # 1568 bytes
        + struct.pack(">H", len(name_bytes))
        + name_bytes
        + kem_sig             # up to 4627 bytes
    )
```

### Pattern 4: Delegation Blob Construction
**What:** Admin grants write access by writing a delegation blob to their namespace.
**Wire format (from C++ PROTOCOL.md):** `[0xDE 0x1E 0x6A 0x7E][delegate_signing_pk:2592]` -- total 2596 bytes.
**The Directory module needs a `delegate()` helper.**

```python
# Source: db/PROTOCOL.md, db/wire/codec.h
DELEGATION_MAGIC = bytes([0xDE, 0x1E, 0x6A, 0x7E])

async def delegate(self, delegate_identity: Identity) -> WriteResult:
    """Grant write access to delegate's signing key in directory namespace."""
    delegation_data = DELEGATION_MAGIC + delegate_identity.public_key
    return await self._client.write_blob(delegation_data, ttl=0)
```

### Pattern 5: Lazy Cache with Dirty Flag
**What:** Cache populated on first read, invalidated by pub/sub notification setting a dirty flag.
**When:** Directory lookups.
**Why:** Directory changes rarely (user registration events). Full cache rebuild is acceptable.

```
First call to list_users() / get_user():
  1. If cache is None (never populated): full scan + subscribe
  2. If _dirty flag set: full scan (re-subscribe not needed -- already subscribed)
  3. Else: return from cache

Notification arrives for directory namespace:
  -> Set _dirty = True (do NOT rebuild immediately)

Next call to any read method:
  -> Sees _dirty, triggers full rebuild
```

### Pattern 6: Notification Observation Without Stealing
**What:** The Directory needs to observe namespace notifications without preventing user code from also seeing them.
**Problem:** Transport has a single notification queue. `client.notifications()` consumes from it. If Directory also consumes, user code misses notifications.
**Recommended approach:** Background asyncio task that reads from the queue, checks namespace, sets dirty flag, and re-enqueues the notification.

```python
async def _notification_watcher(self) -> None:
    """Background task: watch for directory namespace notifications."""
    while not self._client._transport.closed:
        try:
            item = await asyncio.wait_for(
                self._client._transport.notifications.get(),
                timeout=1.0,
            )
            msg_type, payload, request_id = item
            if msg_type == TransportMsgType.Notification:
                notif = decode_notification(payload)
                if notif.namespace == self._directory_namespace:
                    self._dirty = True
            # Re-enqueue for user code consumption
            try:
                self._client._transport.notifications.put_nowait(item)
            except asyncio.QueueFull:
                pass
        except asyncio.TimeoutError:
            continue
```

**Alternative (simpler, recommended):** Do NOT run a background task. Instead, drain pending notifications at the start of each cache-reading method call. This is synchronous with the caller and avoids task management complexity:

```python
async def _check_invalidation(self) -> None:
    """Drain notification queue, set dirty flag if directory namespace seen."""
    requeue = []
    while not self._client._transport.notifications.empty():
        try:
            item = self._client._transport.notifications.get_nowait()
        except asyncio.QueueEmpty:
            break
        msg_type, payload, request_id = item
        if msg_type == TransportMsgType.Notification:
            notif = decode_notification(payload)
            if notif.namespace == self._directory_namespace:
                self._dirty = True
        requeue.append(item)
    # Re-enqueue everything
    for item in requeue:
        try:
            self._client._transport.notifications.put_nowait(item)
        except asyncio.QueueFull:
            break
```

### Anti-Patterns to Avoid
- **Storing KEM public keys in a separate namespace:** Uses existing directory namespace. No special _keys namespace (Anti-Pattern 4 from ARCHITECTURE.md).
- **Including full pubkeys in the Directory class constructor:** Constructor takes client + identity, namespace is derived. No raw key bytes in the public API.
- **Incremental cache update on notification:** Full cache clear is simpler and correct for a rarely-changing directory. Incremental adds complexity for negligible benefit.
- **Modifying client.py for directory features:** Directory is a higher-level abstraction in its own module. Do not add directory methods to ChromatinClient.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Binary serialization | Custom byte manipulation | struct.pack/unpack with big-endian | Consistent with all existing _codec.py patterns |
| Delegation write | Manual blob construction in user code | Directory.delegate() method | Encapsulates DELEGATION_MAGIC + pubkey format |
| Identity reconstruction | Custom key parsing | Identity.from_public_keys(signing_pk, kem_pk) | Already exists from Phase 75, handles validation |
| Signature verification | Manual oqs calls | Identity.verify(message, signature, pubkey) | Static method already in identity.py |
| SHA3-256 hashing | hashlib direct | crypto.sha3_256() | Existing wrapper, used everywhere |
| Pub/sub subscription | Manual transport calls | client.subscribe(namespace) | Already handles fire-and-forget + tracking |

## Common Pitfalls

### Pitfall 1: Delegation Format Mismatch
**What goes wrong:** CONTEXT.md D-12 mentions "4-byte magic DLGT + 32-byte delegate namespace" but the actual C++ protocol uses `[0xDE 0x1E 0x6A 0x7E][delegate_signing_pk:2592]`.
**Why it happens:** D-12 is a simplified description. The actual wire format requires the full 2592-byte ML-DSA-87 public key, not a 32-byte namespace hash.
**How to avoid:** Always reference `db/PROTOCOL.md` line 310 and `db/wire/codec.h` line 66 for the canonical delegation format: `DELEGATION_MAGIC (4 bytes) + delegate_pubkey (2592 bytes) = 2596 bytes total`.
**Warning signs:** Delegation writes succeed (node stores the blob) but delegate's subsequent writes fail (node doesn't recognize the delegation because the data format is wrong).

### Pitfall 2: Notification Queue Contention
**What goes wrong:** Directory consumes notifications from the Transport queue, preventing user code from seeing them via `client.notifications()`.
**Why it happens:** Transport has a single `asyncio.Queue` for all notifications. Consuming from it removes the item.
**How to avoid:** Use the drain-and-requeue pattern: read all pending notifications, check for directory namespace, set dirty flag, put them all back. Or use a background task that re-enqueues after inspection.
**Warning signs:** User's `async for notif in client.notifications()` stops yielding notifications after Directory is initialized.

### Pitfall 3: UserEntry Size Underestimation
**What goes wrong:** Buffer allocation or validation fails because UserEntry is larger than expected.
**Why it happens:** A single UserEntry is approximately 8794 + display_name_len bytes: magic(4) + version(1) + signing_pk(2592) + kem_pk(1568) + name_len(2) + name(N) + kem_sig(4627). That's ~8.6 KB minimum.
**How to avoid:** Size-validate on decode. Minimum size = 4 + 1 + 2592 + 1568 + 2 + 0 + 1 = 4168 bytes (empty name, minimum 1-byte kem_sig). Maximum size = 4 + 1 + 2592 + 1568 + 2 + 256 + 4627 = 9050 bytes.
**Warning signs:** ProtocolError on decode for valid UserEntry blobs.

### Pitfall 4: ML-DSA-87 Signature Variability
**What goes wrong:** Tests expect fixed kem_sig size but get variable-length signatures.
**Why it happens:** ML-DSA-87 signatures are variable length up to 4627 bytes. They are also non-deterministic -- same input produces different signatures each time.
**How to avoid:** UserEntry format puts kem_sig last (after the length-prefixed display_name), so its length is implicitly `total_blob_size - fixed_prefix_size - name_len`. Do NOT add a length prefix for kem_sig -- it is the remainder of the blob.
**Warning signs:** UserEntry roundtrip fails because kem_sig size was hardcoded.

### Pitfall 5: Admin vs Non-Admin Constructor Confusion
**What goes wrong:** Non-admin user tries to delegate or admin tries to self-register.
**Why it happens:** Directory(client, identity) for admin vs Directory(client, identity, directory_namespace=ns) for non-admin look similar.
**How to avoid:** The `directory_namespace` parameter presence determines the mode. If absent, the identity's own namespace IS the directory (admin mode). If present, the identity is a delegate in someone else's directory (non-admin mode). Document clearly. The `delegate()` method should only work in admin mode (identity.namespace == directory_namespace).
**Warning signs:** DirectoryError on operations that should succeed, or successful writes to wrong namespace.

### Pitfall 6: Cache Populated Before Subscribe
**What goes wrong:** Notifications arrive between list_blobs() completing and subscribe() being called. Cache is stale but dirty flag never gets set.
**Why it happens:** Race condition in the populate sequence.
**How to avoid:** Subscribe BEFORE scanning the namespace. Extra notifications (from blobs you're about to read) just trigger an unnecessary rebuild, which is harmless.
**Warning signs:** Stale cache that never invalidates despite new registrations.

## Code Examples

### UserEntry Encode/Decode
```python
# Source: CONTEXT.md D-06, D-07, _codec.py patterns
import struct
from chromatindb.crypto import sha3_256
from chromatindb.identity import Identity, PUBLIC_KEY_SIZE, KEM_PUBLIC_KEY_SIZE

USERENTRY_MAGIC = b"UENT"
USERENTRY_VERSION = 0x01
# Minimum: magic(4) + ver(1) + signing_pk(2592) + kem_pk(1568) + name_len(2) + kem_sig(>=1)
USERENTRY_MIN_SIZE = 4 + 1 + PUBLIC_KEY_SIZE + KEM_PUBLIC_KEY_SIZE + 2 + 1

def encode_user_entry(identity: Identity, display_name: str) -> bytes:
    """Encode a UserEntry blob for directory registration."""
    if not identity.has_kem:
        raise ValueError("Identity must have KEM keypair")
    if not identity.can_sign:
        raise ValueError("Identity must be able to sign")
    name_bytes = display_name.encode("utf-8")
    if len(name_bytes) > 256:
        raise ValueError(f"Display name too long: {len(name_bytes)} > 256 bytes")
    # Sign KEM pubkey with signing key (D-08: prevents MITM key substitution)
    kem_sig = identity.sign(identity.kem_public_key)
    return (
        USERENTRY_MAGIC
        + struct.pack("B", USERENTRY_VERSION)
        + identity.public_key       # 2592 bytes
        + identity.kem_public_key   # 1568 bytes
        + struct.pack(">H", len(name_bytes))
        + name_bytes
        + kem_sig                   # variable, up to 4627 bytes
    )

def decode_user_entry(data: bytes) -> tuple[bytes, bytes, str, bytes] | None:
    """Decode a UserEntry blob. Returns (signing_pk, kem_pk, display_name, kem_sig) or None."""
    if len(data) < USERENTRY_MIN_SIZE:
        return None
    if data[:4] != USERENTRY_MAGIC:
        return None
    version = data[4]
    if version != USERENTRY_VERSION:
        return None
    offset = 5
    signing_pk = data[offset : offset + PUBLIC_KEY_SIZE]
    offset += PUBLIC_KEY_SIZE
    kem_pk = data[offset : offset + KEM_PUBLIC_KEY_SIZE]
    offset += KEM_PUBLIC_KEY_SIZE
    name_len = struct.unpack(">H", data[offset : offset + 2])[0]
    offset += 2
    if offset + name_len > len(data):
        return None
    display_name = data[offset : offset + name_len].decode("utf-8")
    offset += name_len
    kem_sig = data[offset:]  # remainder is kem_sig
    if len(kem_sig) == 0:
        return None
    return signing_pk, kem_pk, display_name, kem_sig
```

### Delegation Blob Construction
```python
# Source: db/PROTOCOL.md line 310, db/wire/codec.h line 66
DELEGATION_MAGIC = bytes([0xDE, 0x1E, 0x6A, 0x7E])

def make_delegation_data(delegate_signing_pk: bytes) -> bytes:
    """Build delegation blob data: [magic:4][delegate_pubkey:2592]."""
    if len(delegate_signing_pk) != PUBLIC_KEY_SIZE:
        raise ValueError(
            f"delegate pubkey must be {PUBLIC_KEY_SIZE} bytes, "
            f"got {len(delegate_signing_pk)}"
        )
    return DELEGATION_MAGIC + delegate_signing_pk
```

### Directory Class Skeleton
```python
# Source: CONTEXT.md D-01 through D-04, D-20 through D-24
from __future__ import annotations
import asyncio
import logging
from dataclasses import dataclass
from chromatindb.client import ChromatinClient
from chromatindb.identity import Identity
from chromatindb.exceptions import ChromatinError

logger = logging.getLogger(__name__)

class DirectoryError(ChromatinError):
    """Directory operation failed."""

@dataclass(frozen=True)
class DirectoryEntry:
    identity: Identity
    display_name: str
    blob_hash: bytes

class Directory:
    def __init__(
        self,
        client: ChromatinClient,
        identity: Identity,
        *,
        directory_namespace: bytes | None = None,
    ) -> None:
        self._client = client
        self._identity = identity
        # Admin mode: own namespace is directory. Non-admin: explicit namespace.
        self._directory_namespace = (
            directory_namespace if directory_namespace is not None
            else identity.namespace
        )
        self._is_admin = directory_namespace is None
        # Cache state
        self._cache: dict[bytes, DirectoryEntry] | None = None  # blob_hash -> entry
        self._by_name: dict[str, DirectoryEntry] = {}
        self._by_pubkey_hash: dict[bytes, DirectoryEntry] = {}
        self._dirty = False
        self._subscribed = False
```

### kem_sig Verification
```python
# Source: CONTEXT.md D-08, D-25; identity.py Identity.verify()
def _verify_user_entry(
    signing_pk: bytes, kem_pk: bytes, kem_sig: bytes
) -> bool:
    """Verify that kem_sig is a valid ML-DSA-87 signature of kem_pk by signing_pk."""
    return Identity.verify(kem_pk, kem_sig, signing_pk)
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Separate key directory namespace | Directory IS admin's existing namespace | v1.7.0 design | No special namespace needed, uses existing delegation |
| Group shared symmetric keys | Per-blob envelope encryption | v1.7.0 design | No key rotation protocol needed |
| from_public_key (signing only) | from_public_keys (signing + KEM) | Phase 75 | Directory entries yield encrypt-capable identities |

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | pytest + pytest-asyncio |
| Config file | sdk/python/pyproject.toml [tool.pytest.ini_options] |
| Quick run command | `cd sdk/python && python -m pytest tests/test_directory.py -x` |
| Full suite command | `cd sdk/python && python -m pytest tests/ -x` |

### Phase Requirements to Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| DIR-01 | Admin creates directory via constructor | unit | `cd sdk/python && python -m pytest tests/test_directory.py::TestDirectoryInit -x` | Wave 0 |
| DIR-02 | User self-registers via delegation + UserEntry write | unit | `cd sdk/python && python -m pytest tests/test_directory.py::TestRegister -x` | Wave 0 |
| DIR-03 | UserEntry binary format with kem_sig | unit | `cd sdk/python && python -m pytest tests/test_directory.py::TestUserEntryCodec -x` | Wave 0 |
| DIR-04 | list_users returns all entries | unit | `cd sdk/python && python -m pytest tests/test_directory.py::TestListUsers -x` | Wave 0 |
| DIR-05 | get_user by name and by pubkey hash | unit | `cd sdk/python && python -m pytest tests/test_directory.py::TestGetUser -x` | Wave 0 |
| DIR-06 | Cache + pub/sub invalidation | unit | `cd sdk/python && python -m pytest tests/test_directory.py::TestCache -x` | Wave 0 |

### Sampling Rate
- **Per task commit:** `cd sdk/python && python -m pytest tests/test_directory.py -x`
- **Per wave merge:** `cd sdk/python && python -m pytest tests/ -x`
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] `sdk/python/tests/test_directory.py` -- all DIR-01 through DIR-06 tests
- [ ] No new fixtures needed beyond existing `conftest.py` (identity fixture already generates KEM keypair)

## Open Questions

1. **Notification queue drain-and-requeue atomicity**
   - What we know: asyncio.Queue get_nowait()/put_nowait() are not atomic across multiple calls. Another coroutine could interleave.
   - What's unclear: Whether this matters in practice, since Directory methods are typically awaited sequentially within a single task.
   - Recommendation: Use drain-and-requeue pattern. The worst case is a notification being processed twice (harmless -- sets dirty flag again) or the user's notifications iterator seeing a slight delay. Not a correctness issue.

2. **list_users() return type: list vs async iterator**
   - What we know: The cache is fully populated on first call. Subsequent calls return from cache.
   - What's unclear: Whether to return `list[DirectoryEntry]` or `AsyncIterator[DirectoryEntry]`.
   - Recommendation: Return `list[DirectoryEntry]`. The data is already fully in memory (cache). An async iterator adds complexity for no benefit. The full list is small (directory users, not millions of records).

3. **Directory as context manager**
   - What we know: Directory needs to subscribe to namespace and potentially manage a background task.
   - What's unclear: Whether __aenter__/__aexit__ is needed for lifecycle management.
   - Recommendation: NO context manager. Keep it as a plain class. Subscribe lazily on first cache populate. No background task to manage (use drain-and-requeue instead). The subscription is cleaned up when the underlying ChromatinClient disconnects (D-06 in client.py auto-unsubscribes on __aexit__). This avoids the need for users to manage two context managers.

## Sources

### Primary (HIGH confidence)
- `sdk/python/chromatindb/identity.py` -- Identity class with from_public_keys(), verify(), has_kem, kem_public_key (Phase 75)
- `sdk/python/chromatindb/types.py` -- Frozen dataclass patterns for all 18 result types
- `sdk/python/chromatindb/_codec.py` -- Binary encode/decode patterns (struct.pack, big-endian, length-prefixed)
- `sdk/python/chromatindb/client.py` -- write_blob, read_blob, list_blobs, subscribe, delegation_list APIs
- `sdk/python/chromatindb/_transport.py` -- Transport.notifications queue, background reader, send_message
- `sdk/python/chromatindb/_envelope.py` -- Envelope encrypt/decrypt (consumers of directory entries)
- `sdk/python/chromatindb/exceptions.py` -- Exception hierarchy (ChromatinError base class)
- `db/PROTOCOL.md` line 307-316 -- Delegation blob format: `[0xDE 0x1E 0x6A 0x7E][delegate_pubkey:2592]`
- `db/wire/codec.h` line 66 -- DELEGATION_MAGIC = {0xDE, 0x1E, 0x6A, 0x7E}
- `.planning/phases/76-directory-user-discovery/76-CONTEXT.md` -- All locked decisions D-01 through D-29

### Secondary (MEDIUM confidence)
- `.planning/research/ARCHITECTURE.md` -- Directory namespace architecture, anti-patterns
- `.planning/research/FEATURES.md` -- Feature analysis, pubkey directory rationale

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- zero new deps, all building blocks exist and are verified in source
- Architecture: HIGH -- patterns directly follow existing SDK conventions, all source code read
- Pitfalls: HIGH -- identified from source code analysis (delegation format, notification queue, signature variability)

**Research date:** 2026-04-01
**Valid until:** 2026-05-01 (stable -- pure SDK work on established patterns)
