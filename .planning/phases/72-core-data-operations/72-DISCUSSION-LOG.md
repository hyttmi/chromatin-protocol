# Phase 72: Core Data Operations - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md -- this log preserves the alternatives considered.

**Date:** 2026-03-29
**Phase:** 72-core-data-operations
**Areas discussed:** Write API surface, Return types, Delete semantics, Error mapping

---

## Write API surface

### Q1: How much should the SDK automate for write_blob?

| Option | Description | Selected |
|--------|-------------|----------|
| Full convenience | User provides (data, ttl). SDK auto-generates timestamp, builds signing input, signs, computes blob_hash, sends Data message. | ✓ |
| Explicit timestamp | User provides (data, ttl, timestamp). No auto-generation. |  |
| Both (optional timestamp) | timestamp is optional kwarg, defaults to int(time.time()) if omitted. |  |

**User's choice:** Full convenience
**Notes:** Least friction for the common case.

### Q2: Should TTL have a default value, or always be required?

| Option | Description | Selected |
|--------|-------------|----------|
| Required | No default TTL -- user must always specify. | ✓ |
| Default 0 (permanent) | Omitting TTL means permanent storage. |  |
| Default 86400 (24h) | Safe default -- blobs expire in a day. |  |

**User's choice:** Required
**Notes:** Prevents accidental permanent blobs or surprise expiry.

### Q3: Should write_blob accept the identity's namespace implicitly?

| Option | Description | Selected |
|--------|-------------|----------|
| Implicit from identity | SDK derives namespace from pubkey. User never passes namespace to write. | ✓ |
| Explicit namespace parameter | User passes namespace. SDK still signs with identity key. |  |

**User's choice:** Implicit from identity
**Notes:** Matches security model -- can only write to own namespace.

### Q4: For read_blob and list_blobs, should the user pass namespace as bytes or hex string?

| Option | Description | Selected |
|--------|-------------|----------|
| Bytes only | Consistent with crypto layer -- namespace is always 32 bytes. | ✓ |
| Accept both | Convenience -- users often copy-paste hex from logs. |  |

**User's choice:** Bytes only
**Notes:** Consistent with identity.namespace returning bytes.

### Q5: Should read/list/exists default to connected identity's namespace?

| Option | Description | Selected |
|--------|-------------|----------|
| Always explicit | User passes namespace for every call. Clear and honest. | ✓ |
| Default to own namespace | Omitting namespace reads from identity's namespace. |  |

**User's choice:** Always explicit
**Notes:** You can read any namespace on the node, not just your own.

### Q6: For list_blobs pagination, what pattern?

| Option | Description | Selected |
|--------|-------------|----------|
| Cursor-based | Returns results + cursor. Pass cursor to next call. | ✓ |
| Async iterator | Yields all blobs across pages automatically. |  |
| Simple offset | list_blobs(ns, offset=0, limit=50). |  |

**User's choice:** Cursor-based
**Notes:** Maps to C++ seq_num-based pagination.

---

## Return types

### Q1: What form should operation results take?

| Option | Description | Selected |
|--------|-------------|----------|
| Typed dataclasses | WriteResult, ReadResult, ListPage, BlobRef. Named fields, IDE autocomplete. | ✓ |
| Raw tuples/bytes | Return raw protocol values. Minimal abstraction. |  |
| TypedDict | Dict-like with type hints. Serializable, JSON-friendly. |  |

**User's choice:** Typed dataclasses
**Notes:** Standard Python SDK pattern.

### Q2: Where should the result dataclasses live?

| Option | Description | Selected |
|--------|-------------|----------|
| chromatindb.types | New module for all result/input types. Clean separation. | ✓ |
| In client.py | Co-located with the methods that return them. |  |

**User's choice:** chromatindb.types
**Notes:** Types don't depend on transport or crypto. Easy to import.

---

## Delete semantics

### Q1: How should delete_blob work from the user's perspective?

| Option | Description | Selected |
|--------|-------------|----------|
| Opaque tombstone | User calls delete_blob(blob_hash). SDK builds tombstone internally. | ✓ |
| Explicit tombstone | User builds tombstone data manually, calls write_blob. |  |
| Delete message type | Use DeleteRequest wire type if it exists. |  |

**User's choice:** Opaque tombstone
**Notes:** SDK uses Delete message type (17) internally, not Data. Node responds with DeleteAck (18).

---

## Error mapping

### Q1: How should protocol-level errors surface?

| Option | Description | Selected |
|--------|-------------|----------|
| None for not-found, raise for failures | read_blob returns None if missing. exists returns bool. Raises ProtocolError for failures. | ✓ |
| Always raise on error | BlobNotFoundError, QuotaExceededError, etc. |  |
| Result types with status | All operations return result objects with status field. |  |

**User's choice:** None for not-found, raise for failures
**Notes:** Python idiom (dict.get returns None).

### Q2: How granular should protocol error types be?

| Option | Description | Selected |
|--------|-------------|----------|
| Single ProtocolError | One exception type with descriptive message. Node doesn't send rich error codes. | ✓ |
| Granular subtypes | QuotaExceededError, RateLimitError, etc. |  |

**User's choice:** Single ProtocolError
**Notes:** V1 SDK, keep simple.

### Q3: When the node silently drops a request, how should the SDK handle it?

| Option | Description | Selected |
|--------|-------------|----------|
| Per-request timeout | Configurable timeout (default 10s). Raises ConnectionError on timeout. | ✓ |
| No timeout (caller wraps) | User wraps with asyncio.wait_for() if they want timeout. |  |

**User's choice:** Per-request timeout
**Notes:** Prevents indefinite hangs from dropped requests.

---

## Claude's Discretion

- Internal payload encoding/decoding helper organization
- Blob FlatBuffer encoding for Data/Delete messages
- Test fixture structure for integration tests
- Exact DeleteResult fields
- ListPage exact field naming

## Deferred Ideas

None -- discussion stayed within phase scope.
