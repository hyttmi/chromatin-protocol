# Phase 76: Directory & User Discovery - Context

**Gathered:** 2026-04-01
**Status:** Ready for planning

<domain>
## Phase Boundary

Admin-owned directory namespace with user self-registration, user listing, pubkey fetch, and cached lookups. Pure SDK work -- no C++ node changes, no new wire types. A new `_directory.py` module wraps existing ChromatinClient primitives (write_blob, read_blob, list_blobs, subscribe, delegation_list) into a higher-level directory API. Users can publish their encryption pubkeys and discover other users' pubkeys for envelope encryption.

</domain>

<decisions>
## Implementation Decisions

### Directory Model
- **D-01:** A directory is a configuration object (e.g., `Directory` class) that wraps a ChromatinClient and an admin namespace. The "directory" IS the admin's namespace -- no special marker blob or init protocol needed
- **D-02:** Admin creates a directory by constructing `Directory(client, admin_identity)` where `admin_identity.namespace` defines the directory namespace
- **D-03:** Non-admin users interact via `Directory(client, user_identity, directory_namespace=admin_namespace)` to read and self-register
- **D-04:** New module: `sdk/python/chromatindb/_directory.py` -- separate from client.py. Directory is a higher-level abstraction on top of the client

### UserEntry Blob Format
- **D-05:** UserEntry is raw binary with length-prefixed fields, consistent with _codec.py patterns. Node stores opaque blobs -- format is SDK-internal only
- **D-06:** UserEntry layout: `[magic:4][version:1][signing_pk:2592][kem_pk:1568][name_len:2 BE][display_name:N][kem_sig:4627]`
- **D-07:** Magic bytes: `UENT` (UserEntry). Version: `0x01`
- **D-08:** `kem_sig` is ML-DSA-87 signature of the KEM public key bytes, signed by the registering user's signing key. Verifiable by any reader using `signing_pk` from the same entry
- **D-09:** Display name is UTF-8 encoded, max 256 bytes. Length-prefixed with uint16 big-endian
- **D-10:** UserEntry is written as a regular signed blob to the directory namespace. The blob-level ML-DSA-87 signature (standard ingest) proves the writer owns their signing key. The kem_sig inside proves the writer owns the KEM pubkey

### Registration Flow
- **D-11:** Two-step: (1) Admin delegates write access to user's signing pubkey for the directory namespace, (2) User self-registers by writing a UserEntry blob
- **D-12:** Admin delegation uses existing `write_blob()` with delegation encoding (4-byte magic `DLGT` + 32-byte delegate namespace). Already implemented in SDK
- **D-13:** `Directory.register(display_name)` method: builds UserEntry, calls `client.write_blob()`. Returns the blob hash
- **D-14:** Registration is idempotent -- writing the same UserEntry again just returns duplicate=True from the node

### User Discovery
- **D-15:** `Directory.list_users()` returns all UserEntry entries in the directory namespace. Uses `client.list_blobs()` + `client.read_blob()` for each
- **D-16:** `Directory.get_user(display_name)` returns a single UserEntry by display name match. Returns None if not found
- **D-17:** `Directory.get_user_by_pubkey(pubkey_hash)` returns a UserEntry by SHA3-256(signing_pk) match. Returns None if not found
- **D-18:** Both lookup methods return an `Identity` object (via `Identity.from_public_keys()`) so the caller can immediately use it for encryption. Also return display_name metadata
- **D-19:** Return type for user lookups: `DirectoryEntry` frozen dataclass with `identity: Identity`, `display_name: str`, `blob_hash: bytes`

### Caching (DIR-06)
- **D-20:** Directory holds an in-memory cache: `dict[bytes, DirectoryEntry]` keyed by blob_hash
- **D-21:** Cache is populated lazily on first `list_users()` or `get_user()` call. Full namespace scan, decode each blob, build index
- **D-22:** Subscribe to directory namespace on cache populate. On notification, clear entire cache (not incremental). Next access triggers full refresh
- **D-23:** `Directory.refresh()` method for explicit cache invalidation
- **D-24:** Build secondary indexes on populate: `_by_name: dict[str, DirectoryEntry]` and `_by_pubkey_hash: dict[bytes, DirectoryEntry]` for O(1) lookup

### Validation
- **D-25:** On cache populate, verify each UserEntry's kem_sig: `Identity.verify(kem_pk, kem_sig, signing_pk)`. Skip entries that fail verification (log warning, don't cache)
- **D-26:** On register(), verify the user identity has KEM keypair (`has_kem` must be True). Raise ValueError if not
- **D-27:** On get_user/get_user_by_pubkey, return only verified entries from cache (never return unverified)

### Error Handling
- **D-28:** New exception: `DirectoryError(ChromatinError)` for directory-specific failures (e.g., user not delegated, namespace not found)
- **D-29:** Registration without prior delegation fails at the node level (write rejected) -- SDK raises DirectoryError wrapping the node rejection

### Claude's Discretion
- Internal cache data structures and index rebuild strategy
- Whether Directory uses __aenter__/__aexit__ or just regular class
- Exact test case breakdown and assertion patterns
- Whether list_users() returns list or async iterator
- Notification callback mechanism (background task vs on-demand check)

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Existing SDK modules (use directly)
- `sdk/python/chromatindb/client.py` -- write_blob, read_blob, list_blobs, subscribe, delegation_list (all needed for directory operations)
- `sdk/python/chromatindb/identity.py` -- Identity.from_public_keys() for reconstructing directory entries, Identity.verify() for kem_sig validation
- `sdk/python/chromatindb/types.py` -- Frozen dataclass conventions (WriteResult, ReadResult, ListPage, BlobRef)
- `sdk/python/chromatindb/_codec.py` -- Binary encode/decode patterns with struct.pack (reference for UserEntry encoding)
- `sdk/python/chromatindb/exceptions.py` -- Exception hierarchy (add DirectoryError here)
- `sdk/python/chromatindb/_envelope.py` -- envelope_encrypt recipient interface (directory entries feed into this)

### Existing SDK modules (reference patterns)
- `sdk/python/chromatindb/_transport.py` -- Background reader pattern (reference for notification handling)
- `sdk/python/chromatindb/_handshake.py` -- Delegation magic bytes reference (DLGT)

### Existing tests (reference patterns)
- `sdk/python/tests/test_identity.py` -- Identity test patterns
- `sdk/python/tests/test_envelope.py` -- Crypto module test patterns
- `sdk/python/tests/conftest.py` -- Fixtures: identity, tmp_dir, load_vectors

### Protocol references
- `db/PROTOCOL.md` -- Delegation blob format (magic DLGT + delegate namespace), subscription semantics
- `.planning/research/ARCHITECTURE.md` -- Directory service design rationale
- `.planning/research/FEATURES.md` -- Directory feature prioritization

### Requirements
- `.planning/REQUIREMENTS.md` -- DIR-01 through DIR-06

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `client.write_blob()`: Write signed blobs (UserEntry blobs to directory namespace)
- `client.read_blob()`: Fetch blob data by hash
- `client.list_blobs()`: Paginated namespace enumeration (directory listing)
- `client.subscribe()`: Real-time namespace notifications (cache invalidation trigger)
- `client.delegation_list()`: Check existing delegations for a namespace
- `Identity.from_public_keys()`: Reconstruct encrypt-capable Identity from directory entries
- `Identity.verify()`: Verify ML-DSA-87 signatures (validate kem_sig in UserEntry)
- `_codec.py` struct.pack patterns: Binary encoding for UserEntry format

### Established Patterns
- Frozen dataclasses in types.py for result types (follow for DirectoryEntry)
- Exception hierarchy under ChromatinError (follow for DirectoryError)
- Private module convention (_directory.py) with re-exports from __init__.py
- Length-prefixed binary encoding with big-endian integers (_codec.py)

### Integration Points
- `__init__.py`: Re-export Directory class and DirectoryEntry type
- `exceptions.py`: Add DirectoryError
- `types.py` or `_directory.py`: Add DirectoryEntry dataclass
- client.py subscription callbacks: Directory needs to handle notification events for cache invalidation

</code_context>

<specifics>
## Specific Ideas

- UserEntry kem_sig prevents MITM key substitution: if attacker replaces kem_pk in a UserEntry, the kem_sig won't verify against the signing_pk, so the entry is rejected
- Delegation is the gatekeeper: only users delegated by admin can write to the directory namespace. The node enforces this at ingest time
- Directory entries are just blobs: they replicate, sync, and expire using standard node mechanisms. No special protocol support needed
- Cache clear on notification is simple and correct: directory doesn't change often (user registration, not chat). Incremental updates add complexity for negligible benefit
- from_public_keys() was specifically designed in Phase 75 for this use case (D-26 in Phase 75 CONTEXT.md)

</specifics>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope

</deferred>

---

*Phase: 76-directory-user-discovery*
*Context gathered: 2026-04-01*
