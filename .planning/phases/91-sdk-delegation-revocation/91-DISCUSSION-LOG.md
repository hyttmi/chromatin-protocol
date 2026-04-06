# Phase 91: SDK Delegation Revocation - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md -- this log preserves the alternatives considered.

**Date:** 2026-04-06
**Phase:** 91-sdk-delegation-revocation
**Areas discussed:** Revocation API surface, Error semantics, Testing strategy

---

## Revocation API Surface

### Q1: Where should revoke_delegate() live?

| Option | Description | Selected |
|--------|-------------|----------|
| Directory.revoke_delegation() | Mirrors Directory.delegate() -- keeps delegation lifecycle in one place. Looks up blob hash via delegation_list(), then calls client.delete_blob(). Admin-mode check. | ✓ |
| Client.revoke_delegation() | Lower-level, on the client alongside delegation_list() and delete_blob(). Caller must be in the right namespace. | |

**User's choice:** Directory.revoke_delegation() (Recommended)
**Notes:** None

### Q2: What should revoke_delegation() accept as input?

| Option | Description | Selected |
|--------|-------------|----------|
| Identity only | revoke_delegation(delegate_identity: Identity) -- method computes pk_hash internally, looks up delegation_blob_hash via delegation_list(), tombstones it. Mirrors delegate(delegate_identity) signature. | ✓ |
| Identity or blob_hash | Overloaded: accept either an Identity (lookup + tombstone) or a raw 32-byte delegation_blob_hash (skip lookup, tombstone directly). | |
| pk_hash bytes | revoke_delegation(delegate_pk_hash: bytes) -- accepts 32-byte SHA3 hash of delegate's pubkey. Matches DelegationEntry.delegate_pk_hash format. | |

**User's choice:** Identity only (Recommended)
**Notes:** None

### Q3: Should Directory also expose a list_delegates() convenience method?

| Option | Description | Selected |
|--------|-------------|----------|
| Yes -- list_delegates() | Returns list of DelegationEntry from the directory namespace. Wraps client.delegation_list(self._namespace). Completes the delegate/revoke/list triad. | ✓ |
| No -- use client.delegation_list() directly | Caller uses the lower-level client.delegation_list(namespace) already shipped. Avoids adding surface. | |

**User's choice:** Yes -- list_delegates() (Recommended)
**Notes:** None

---

## Error Semantics

### Q1: What should happen when revoking a delegate that has no active delegation?

| Option | Description | Selected |
|--------|-------------|----------|
| Raise DelegationNotFoundError | Explicit error -- caller knows the revocation didn't happen because no delegation existed. Subclass of DirectoryError. | ✓ |
| Silent no-op, return None | Idempotent -- revoking an already-revoked delegate is harmless. Returns None instead of DeleteResult. | |
| Return a status result | Return a RevocationResult with a found/not_found flag. No exception, caller checks result. | |

**User's choice:** Raise DelegationNotFoundError (Recommended)
**Notes:** None

### Q2: If the tombstone write itself fails, should exceptions propagate?

| Option | Description | Selected |
|--------|-------------|----------|
| Let exceptions propagate | ProtocolError / ConnectionError from delete_blob() bubble up unchanged. Consistent with delegate() and all other Directory methods. | ✓ |
| Wrap in RevocationError | Catch and re-raise as a RevocationError with the original as cause. Gives caller a single exception type. | |

**User's choice:** Let exceptions propagate (Recommended)
**Notes:** None

---

## Testing Strategy

### Q1: What testing approach for delegation revocation?

| Option | Description | Selected |
|--------|-------------|----------|
| Unit + KVM integration | Unit tests for revoke_delegation(), list_delegates(), error paths. KVM swarm integration test for multi-node propagation (REV-02). | ✓ |
| Unit tests only | Fast, no Docker dependency. Mock client responses. Can't verify node enforcement. | |
| Docker integration only | E2E proof across nodes. Slower, directly verifies REV-01 and REV-02. | |

**User's choice:** Unit + KVM node tests
**Notes:** User specified KVM swarm (not Docker) for integration tests. Needs to update all nodes + relays before running.

### Q2: Should the test script handle deploying updated binaries?

| Option | Description | Selected |
|--------|-------------|----------|
| Separate manual step | Test assumes nodes are already running latest binaries. Deploy first, then run SDK integration tests. | ✓ |
| Test script deploys | Test script SSHes into KVM guests, copies binaries, restarts services, then runs SDK tests. | |

**User's choice:** Separate manual step (Recommended)
**Notes:** None

---

## Claude's Discretion

- Internal implementation details of delegation blob hash lookup flow
- Test fixture organization and helper structure
- Exact exception message wording

## Deferred Ideas

None -- discussion stayed within phase scope
