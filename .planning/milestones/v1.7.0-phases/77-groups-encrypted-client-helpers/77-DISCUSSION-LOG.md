# Phase 77: Groups & Encrypted Client Helpers - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md -- this log preserves the alternatives considered.

**Date:** 2026-04-02
**Phase:** 77-groups-encrypted-client-helpers
**Areas discussed:** Group blob format, Helper placement, write_encrypted API, Group admin ops

---

## Group Blob Format

### Member Representation

| Option | Description | Selected |
|--------|-------------|----------|
| 32-byte pubkey hashes | SHA3-256(signing_pk) per member. Compact, directory lookup resolves to KEM pubkey at encrypt-time | ✓ |
| Full 2592-byte signing pubkeys | Self-contained but huge group blobs, still need directory for KEM pubkeys | |

**User's choice:** 32-byte pubkey hashes
**Notes:** Compact representation preferred. Directory lookup is required for KEM pubkeys regardless.

### Group Update Semantics

| Option | Description | Selected |
|--------|-------------|----------|
| Latest-timestamp wins | Multiple group blobs coexist, SDK picks highest timestamp. Simple, no tombstones | ✓ |
| Delete-and-replace | Tombstone old blob, write new one. Extra round-trip | |

**User's choice:** Latest-timestamp wins
**Notes:** None

### Binary Layout

| Option | Description | Selected |
|--------|-------------|----------|
| GRPE + name + member hashes | [GRPE:4][version:1][name_len:2 BE][name:N][member_count:2 BE][N x hash:32] | ✓ |
| You decide | Claude picks layout | |

**User's choice:** GRPE + name + member hashes
**Notes:** Follows UserEntry pattern.

### Cache Integration

| Option | Description | Selected |
|--------|-------------|----------|
| Same Directory cache | Extend _populate_cache() to decode both UENT and GRPE blobs in one scan | ✓ |
| Separate group cache | Independent scan/cache lifecycle for groups | |

**User's choice:** Same Directory cache
**Notes:** Single namespace scan populates both user and group indexes.

---

## Helper Placement

### Method Location

| Option | Description | Selected |
|--------|-------------|----------|
| On ChromatinClient directly | Add write_encrypted/read_encrypted/write_to_group as methods. Matches roadmap | ✓ |
| Separate EncryptedClient wrapper | New class wrapping client + directory. Users juggle two objects | |

**User's choice:** On ChromatinClient directly
**Notes:** None

### Directory Access for write_to_group

| Option | Description | Selected |
|--------|-------------|----------|
| Pass Directory per-call | write_to_group(data, group_name, directory, ttl=...). Explicit, no hidden state | ✓ |
| ChromatinClient holds a Directory | Optional .directory property. Convenient but state coupling | |
| You decide | Claude picks cleanest approach | |

**User's choice:** Pass Directory per-call
**Notes:** None

---

## write_encrypted API

### TTL Handling

| Option | Description | Selected |
|--------|-------------|----------|
| Required parameter | write_encrypted(data, recipients, ttl=3600). Matches write_blob | ✓ |
| Optional with default | ttl=0 default (permanent). Could surprise users | |

**User's choice:** Required parameter
**Notes:** None

### Return Type

| Option | Description | Selected |
|--------|-------------|----------|
| WriteResult | Same as write_blob. Consistent, preserves seq_num and duplicate info | ✓ |
| Just blob_hash bytes | Simpler but loses metadata | |

**User's choice:** WriteResult
**Notes:** None

### Not-a-Recipient Behavior

| Option | Description | Selected |
|--------|-------------|----------|
| Raise NotARecipientError | Distinct from not-found. Matches Phase 75 envelope_decrypt | ✓ |
| Return None | Treat same as not-found. Simpler but hides info | |

**User's choice:** Raise NotARecipientError
**Notes:** None

### Self-Encrypt

| Option | Description | Selected |
|--------|-------------|----------|
| recipients=None default | write_encrypted(data, ttl=3600) with no recipients = self-only | ✓ |
| Explicit empty list | Must pass recipients=[] to self-encrypt | |

**User's choice:** recipients=None default
**Notes:** None

---

## Group Admin Ops

### Group Creation Authority

| Option | Description | Selected |
|--------|-------------|----------|
| Admin-only | Only admin mode Directory can create/update groups | ✓ |
| Any delegated user | Anyone delegated can create groups | |

**User's choice:** Admin-only
**Notes:** Consistent with delegation model.

### GroupEntry Type

| Option | Description | Selected |
|--------|-------------|----------|
| Frozen dataclass | GroupEntry(name, members, blob_hash, timestamp). Follows DirectoryEntry pattern | ✓ |
| Dict/tuple | Raw data without dedicated type | |

**User's choice:** Frozen dataclass
**Notes:** None

### Mutation Approach

| Option | Description | Selected |
|--------|-------------|----------|
| Read-modify-write | Read current group, modify member list, write new blob. SDK handles immutable-blob dance | ✓ |
| Full replacement only | No add/remove helpers, user provides full list each time | |

**User's choice:** Read-modify-write
**Notes:** None

### Member Identification in API

| Option | Description | Selected |
|--------|-------------|----------|
| Identity object | add_member(group_name, identity). SDK extracts hash. Consistent with envelope_encrypt | ✓ |
| Raw 32-byte pubkey hash | Lower-level, user computes hash | |
| Either (overloaded) | Accept Identity or bytes at runtime | |

**User's choice:** Identity object
**Notes:** None

---

## Claude's Discretion

- GroupEntry placement (in _directory.py or types.py)
- encode/decode_group_entry helper structure
- list_groups return type and method placement
- Test case breakdown
- Group resolution path in write_to_group

## Deferred Ideas

None -- discussion stayed within phase scope
