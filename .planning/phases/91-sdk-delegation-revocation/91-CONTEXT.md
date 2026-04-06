# Phase 91: SDK Delegation Revocation - Context

**Gathered:** 2026-04-06
**Status:** Ready for planning

<domain>
## Phase Boundary

Owners can revoke a delegate's write access through the Python SDK and verify that the node enforces it immediately. Uses existing node tombstone mechanism (proven in Docker `test_acl04_revocation.sh`). All SDK-only Python changes: zero C++ node modifications, zero new wire types.

</domain>

<decisions>
## Implementation Decisions

### Revocation API Surface
- **D-01:** `Directory.revoke_delegation(delegate_identity: Identity)` — lives on Directory next to `delegate()`. Mirrors the delegation lifecycle in one place. Admin-mode check required.
- **D-02:** Input is `Identity` only — method computes pk_hash internally, looks up `delegation_blob_hash` via `delegation_list()`, then tombstones it via `delete_blob()`.
- **D-03:** `Directory.list_delegates()` convenience method — wraps `client.delegation_list(self._namespace)`, returns list of `DelegationEntry`. Completes the delegate/revoke/list triad.

### Error Handling
- **D-04:** Raise `DelegationNotFoundError` (subclass of `DirectoryError`) when revoking a delegate that has no active delegation (already revoked or never delegated).
- **D-05:** Let existing exceptions propagate unchanged — `ProtocolError` / `ConnectionError` from `delete_blob()` bubble up as-is. Consistent with `delegate()` and all other Directory methods.

### Testing Strategy
- **D-06:** Unit tests in `test_directory.py` for `revoke_delegation()`, `list_delegates()`, error paths (not found, admin check, propagation of write errors).
- **D-07:** Integration tests against live KVM 3-node swarm (192.168.1.200-202) — SDK-driven multi-node propagation test verifying REV-02 (revoked delegate's writes rejected after tombstone sync).
- **D-08:** Manual deploy step — test assumes nodes/relays are already running latest binaries. Deployment to KVM guests is a separate step before running integration tests.

### Claude's Discretion
- Internal implementation details of the delegation blob hash lookup flow
- Test fixture organization and helper structure
- Exact exception message wording

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Protocol & Architecture
- `.planning/REQUIREMENTS.md` — REV-01, REV-02 requirements for this phase
- `.planning/research/SUMMARY.md` — Architecture approach, recommended stack, feature breakdown
- `.planning/research/PITFALLS.md` — Critical pitfalls: propagation window, tombstone ordering race, TTL=0 permanence

### Existing Implementation
- `sdk/python/chromatindb/_directory.py` — `delegate()` method (line ~330), `make_delegation_data()` (line ~263). New methods go here.
- `sdk/python/chromatindb/client.py` — `delete_blob()` (line ~514), `delegation_list()` (line ~865). Primitives used by revocation.
- `sdk/python/chromatindb/types.py` — `DelegationEntry`, `DelegationList`, `DeleteResult` dataclasses.
- `sdk/python/chromatindb/_codec.py` — `encode_delegation_list_request()`, `decode_delegation_list_response()`.

### Tests & Verification
- `tests/integration/test_acl04_revocation.sh` — Existing Docker integration test for delegation revocation (C++ loadgen-driven). Pattern reference for SDK integration test.
- `sdk/python/tests/test_directory.py` — Existing delegation tests (line ~172+). New unit tests go here.
- `sdk/python/tests/test_client_ops.py` — Delegation list tests (line ~1431+). Reference for mocking patterns.

### Node Enforcement (read-only reference)
- `.planning/milestones/v1.0.0-phases/48-access-control-topology/48-VERIFICATION.md` — Verified: tombstoned delegation syncs to all peers, revoked delegate writes rejected.

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `Directory.delegate(delegate_identity)` — Pattern for admin-mode write, returns WriteResult
- `client.delete_blob(blob_hash)` — Tombstone creation primitive, returns DeleteResult
- `client.delegation_list(namespace)` — Node query returning DelegationEntry list with delegate_pk_hash + delegation_blob_hash
- `make_delegation_data()` — Delegation blob format (magic:4 + delegate_pubkey:2592)
- `Identity.namespace` — SHA3-256(signing_pk), used as directory namespace

### Established Patterns
- Directory methods check `self._is_admin` before admin-only operations
- Directory uses `self._client` for all wire operations
- All errors inherit from `DirectoryError` or `ProtocolError`
- Namespace is always 32 bytes (SHA3-256 hash)
- DelegationEntry has `delegate_pk_hash` (32 bytes) and `delegation_blob_hash` (32 bytes)

### Integration Points
- `_directory.py` — `revoke_delegation()` and `list_delegates()` methods next to `delegate()`
- `types.py` — New `DelegationNotFoundError` exception class
- `__init__.py` — Export new methods and exception
- Test files: `test_directory.py`, `test_integration.py`

</code_context>

<specifics>
## Specific Ideas

- Revocation flow: `revoke_delegation(identity)` -> compute `SHA3-256(identity.public_key)` -> call `delegation_list()` -> find matching `delegate_pk_hash` -> `delete_blob(delegation_blob_hash)` -> return `DeleteResult`
- KVM swarm integration test should verify: (1) delegate can write, (2) owner revokes, (3) delegate write is rejected, (4) second node also rejects after propagation

</specifics>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope

</deferred>

---

*Phase: 91-sdk-delegation-revocation*
*Context gathered: 2026-04-06*
