# Phase 91: SDK Delegation Revocation - Research

**Researched:** 2026-04-06
**Domain:** Python SDK delegation revocation -- adding `revoke_delegation()` and `list_delegates()` to the Directory class, using existing node tombstone mechanism
**Confidence:** HIGH

## Summary

This phase adds two methods to the Python SDK's `Directory` class: `revoke_delegation(delegate_identity)` and `list_delegates()`. The node-layer delegation revocation mechanism is fully implemented and battle-tested in Docker integration tests (`test_acl04_revocation.sh`). The SDK work is pure plumbing: call `client.delegation_list()` to find the delegation blob hash for a given delegate's public key hash, then call `client.delete_blob()` to tombstone it. Zero new wire types, zero C++ changes, zero new dependencies.

The implementation touches four files: `_directory.py` (two new methods), `exceptions.py` (one new exception class), `__init__.py` (export the new exception), and `tests/test_directory.py` (unit tests). An integration test in `tests/test_integration.py` verifies the full revoke-then-reject flow against the live KVM swarm.

**Primary recommendation:** Implement `revoke_delegation()` as a three-step lookup-then-tombstone flow on Directory, add `list_delegates()` as a thin wrapper around `client.delegation_list()`, and add `DelegationNotFoundError` to the exception hierarchy. Unit tests mock `delegation_list` and `delete_blob`; integration test exercises real revocation against 3-node KVM swarm with relay.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** `Directory.revoke_delegation(delegate_identity: Identity)` -- lives on Directory next to `delegate()`. Mirrors the delegation lifecycle in one place. Admin-mode check required.
- **D-02:** Input is `Identity` only -- method computes pk_hash internally, looks up `delegation_blob_hash` via `delegation_list()`, then tombstones it via `delete_blob()`.
- **D-03:** `Directory.list_delegates()` convenience method -- wraps `client.delegation_list(self._namespace)`, returns list of `DelegationEntry`. Completes the delegate/revoke/list triad.
- **D-04:** Raise `DelegationNotFoundError` (subclass of `DirectoryError`) when revoking a delegate that has no active delegation (already revoked or never delegated).
- **D-05:** Let existing exceptions propagate unchanged -- `ProtocolError` / `ConnectionError` from `delete_blob()` bubble up as-is. Consistent with `delegate()` and all other Directory methods.
- **D-06:** Unit tests in `test_directory.py` for `revoke_delegation()`, `list_delegates()`, error paths (not found, admin check, propagation of write errors).
- **D-07:** Integration tests against live KVM 3-node swarm (192.168.1.200-202) -- SDK-driven multi-node propagation test verifying REV-02 (revoked delegate's writes rejected after tombstone sync).
- **D-08:** Manual deploy step -- test assumes nodes/relays are already running latest binaries. Deployment to KVM guests is a separate step before running integration tests.

### Claude's Discretion
- Internal implementation details of the delegation blob hash lookup flow
- Test fixture organization and helper structure
- Exact exception message wording

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| REV-01 | Owner can revoke a delegate's write access via SDK `revoke_delegate()` method (tombstones the delegation blob) | `Directory.revoke_delegation()` calls `client.delegation_list()` to find delegation_blob_hash, then `client.delete_blob()` to tombstone it. Both primitives exist and are tested. |
| REV-02 | Node rejects writes from revoked delegates immediately for connected peers and after sync propagation for disconnected peers | Node enforcement already proven in `test_acl04_revocation.sh`. Integration test against KVM swarm verifies SDK-driven flow end-to-end including multi-node propagation. |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| chromatindb SDK | current (in-tree) | Client library under sdk/python/ | The codebase being modified |
| pytest | 9.0.2 | Test framework | Already installed in sdk/python/.venv |
| pytest-asyncio | (installed) | Async test support | `asyncio_mode = "auto"` in pyproject.toml |

### Supporting
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| unittest.mock | stdlib | AsyncMock, MagicMock for unit tests | All unit tests mock the ChromatinClient |
| liboqs-python | (installed) | ML-DSA-87 signing, ML-KEM-1024 | Identity.generate() in test fixtures |

### Alternatives Considered
None. This phase uses only existing in-tree code and dependencies.

**Installation:** No new packages needed. Everything is already installed in sdk/python/.venv.

**Test run command:**
```bash
cd sdk/python && .venv/bin/python -m pytest tests/test_directory.py -x -v
```

## Architecture Patterns

### File Modification Map
```
sdk/python/chromatindb/
  _directory.py      # ADD: revoke_delegation(), list_delegates()
  exceptions.py      # ADD: DelegationNotFoundError(DirectoryError)
  __init__.py        # ADD: export DelegationNotFoundError
sdk/python/tests/
  test_directory.py  # ADD: TestRevokeDelegation, TestListDelegates classes
  test_integration.py # ADD: test_delegation_revocation_propagation()
```

### Pattern 1: Admin-Gated Directory Method
**What:** All admin-only Directory methods follow the same guard pattern.
**When to use:** Both `revoke_delegation()` and `list_delegates()` are admin-only operations.
**Example:**
```python
# Source: sdk/python/chromatindb/_directory.py line 345, 512
async def revoke_delegation(self, delegate_identity: Identity) -> DeleteResult:
    if not self._is_admin:
        raise DirectoryError("only admin can revoke delegations")
    # ... implementation
```

### Pattern 2: Lookup-Then-Operate (Delegation Hash Resolution)
**What:** `revoke_delegation()` must find the delegation_blob_hash before it can tombstone it. The flow is: compute `SHA3-256(delegate_identity.public_key)` to get the pk_hash, call `client.delegation_list(self._directory_namespace)` to get all active delegations, find the entry where `delegate_pk_hash` matches, then call `client.delete_blob(entry.delegation_blob_hash)`.
**When to use:** This is the only approach -- the node's DelegationListRequest (types 51/52) is the only wire type that maps delegate public key hash to delegation blob hash.
**Example:**
```python
# Revocation flow per D-02 and CONTEXT.md specifics
pk_hash = sha3_256(delegate_identity.public_key)
delegation_result = await self._client.delegation_list(self._directory_namespace)
match = None
for entry in delegation_result.entries:
    if entry.delegate_pk_hash == pk_hash:
        match = entry
        break
if match is None:
    raise DelegationNotFoundError(
        "no active delegation found for delegate"
    )
return await self._client.delete_blob(match.delegation_blob_hash)
```

### Pattern 3: Mock Client Unit Test Pattern
**What:** All Directory unit tests create a MagicMock client via `make_mock_client()`, configure return values for the async methods, instantiate `Directory(client, identity)` in admin mode, and assert on call args.
**When to use:** All new unit tests for `revoke_delegation()` and `list_delegates()`.
**Example:**
```python
# Source: sdk/python/tests/test_directory.py line 236-247
def make_mock_client(identity: Identity) -> MagicMock:
    client = MagicMock()
    client._identity = identity
    client.write_blob = AsyncMock()
    client.read_blob = AsyncMock()
    client.list_blobs = AsyncMock()
    client.subscribe = AsyncMock()
    client._transport = MagicMock()
    client._transport.notifications = asyncio.Queue(maxsize=1000)
    client._transport.closed = False
    return client
```
For new tests, also need `client.delegation_list = AsyncMock()` and `client.delete_blob = AsyncMock()`.

### Pattern 4: Integration Test with Relay Skip
**What:** Integration tests use `pytest.mark.integration` and skip if relay is unreachable.
**When to use:** The KVM swarm revocation propagation test.
**Example:**
```python
# Source: sdk/python/tests/test_integration.py line 56-62
pytestmark = [
    pytest.mark.integration,
    pytest.mark.skipif(
        not _relay_reachable(),
        reason=f"relay at {RELAY_HOST}:{RELAY_PORT} unreachable",
    ),
]
```

### Anti-Patterns to Avoid
- **Calling delete_blob with a pk_hash instead of delegation_blob_hash:** The delegation_blob_hash is the hash of the delegation blob itself (the one containing DELEGATION_MAGIC + delegate_pubkey). The pk_hash is the SHA3-256 of the delegate's signing public key. These are different 32-byte values. The `delete_blob()` requires the blob_hash, not the pk_hash.
- **Bypassing DelegationListRequest:** Do not attempt to compute the delegation_blob_hash locally from the delegation data bytes. The node is authoritative. Always use `delegation_list()` to find the correct blob_hash.
- **Adding new mock methods globally:** Add `delegation_list` and `delete_blob` mocks only in tests that need them, or add to `make_mock_client()` if the majority of tests would benefit (but beware of changing existing test expectations).

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Delegation blob hash lookup | Manual hash computation from delegation data | `client.delegation_list()` (wire types 51/52) | Node is authoritative for active delegations; local computation can't account for already-tombstoned entries |
| Tombstone creation | Custom tombstone wire encoding | `client.delete_blob(blob_hash)` | Already handles tombstone_data encoding, signing, TTL=0, and DeleteAck decoding |
| Public key hash | Custom hashing | `sha3_256(identity.public_key)` from `chromatindb.crypto` | Consistent with all other pk_hash computation in the SDK |

## Common Pitfalls

### Pitfall 1: Confusing delegate_pk_hash with delegation_blob_hash
**What goes wrong:** Using `SHA3-256(delegate_signing_pk)` as the argument to `delete_blob()`. This would attempt to tombstone a blob with that hash, which is NOT the delegation blob.
**Why it happens:** Both are 32-byte hashes. The `DelegationEntry` has two fields (`delegate_pk_hash` and `delegation_blob_hash`) and it is easy to mix them up.
**How to avoid:** The `revoke_delegation()` method computes pk_hash for lookup (matching against `entry.delegate_pk_hash`) but passes `entry.delegation_blob_hash` to `delete_blob()`. Variable naming should be explicit.
**Warning signs:** `delete_blob(sha3_256(delegate_identity.public_key))` -- this is always wrong.

### Pitfall 2: Revocation Propagation Window
**What goes wrong:** Tests or documentation claim revocation is "instant across all nodes." It is not -- tombstone replication is eventually consistent.
**Why it happens:** Connected peers get BlobNotify in sub-second time, giving the illusion of instant propagation. Disconnected peers wait up to safety-net interval (600s default).
**How to avoid:** Integration test uses `asyncio.sleep()` with a reasonable wait (e.g., 5-10 seconds) for a 3-node LAN swarm where all nodes are connected. Document the propagation window explicitly.
**Warning signs:** Fixed zero-delay assertions on multi-node propagation.

### Pitfall 3: Delegation Tombstones Must Be Permanent (TTL=0)
**What goes wrong:** If delegation tombstones ever expire, a delayed delegation blob arriving after expiry could repopulate `delegation_map` with a revoked delegate.
**Why it happens:** The node's ingest pipeline checks `has_tombstone_for()` before storing blobs (step 3.5). If the tombstone expires, this check passes and the delegation blob gets stored.
**How to avoid:** `delegate()` already uses `ttl=0`. `delete_blob()` internally uses `ttl=0` for tombstones. This is correct. Do not change it.
**Warning signs:** Any proposal to add TTL to delegation tombstones.

### Pitfall 4: make_mock_client Missing New Methods
**What goes wrong:** New tests call `client.delegation_list()` or `client.delete_blob()` on a mock that doesn't have AsyncMock configured for those methods. MagicMock creates sync mocks by default, which fail in async contexts.
**Why it happens:** `make_mock_client()` only sets up `write_blob`, `read_blob`, `list_blobs`, `subscribe` as AsyncMock. Other methods are plain MagicMock attributes.
**How to avoid:** Either update `make_mock_client()` to include `delegation_list = AsyncMock()` and `delete_blob = AsyncMock()`, or configure them in each test that needs them.
**Warning signs:** `TypeError: object MagicMock can't be used in 'await' expression`.

### Pitfall 5: list_delegates() Returning Wrong Namespace
**What goes wrong:** Using `self._identity.namespace` instead of `self._directory_namespace` when calling `delegation_list()`. In user mode, these differ.
**Why it happens:** Admin mode sets `_directory_namespace = identity.namespace`. User mode sets `_directory_namespace = directory_namespace` (the admin's namespace). The `delegation_list()` must always query the directory namespace, not the current identity's namespace.
**How to avoid:** Always use `self._directory_namespace` for the delegation_list call. Note: `list_delegates()` is admin-only per D-03, but using `_directory_namespace` is still correct for consistency.
**Warning signs:** Delegation list comes back empty even though delegations exist.

## Code Examples

### revoke_delegation() Implementation Pattern
```python
# Source: derived from CONTEXT.md D-01, D-02, D-04, D-05 and existing patterns
async def revoke_delegation(self, delegate_identity: Identity) -> DeleteResult:
    """Revoke a delegate's write access by tombstoning their delegation blob.

    Args:
        delegate_identity: Identity of the delegate to revoke.

    Returns:
        DeleteResult from the tombstone write.

    Raises:
        DirectoryError: If not in admin mode.
        DelegationNotFoundError: If no active delegation exists for delegate.
        ProtocolError: If the node rejects the tombstone.
        ConnectionError: If the request times out.
    """
    if not self._is_admin:
        raise DirectoryError("only admin can revoke delegations")
    pk_hash = sha3_256(delegate_identity.public_key)
    delegation_result = await self._client.delegation_list(
        self._directory_namespace
    )
    for entry in delegation_result.entries:
        if entry.delegate_pk_hash == pk_hash:
            return await self._client.delete_blob(
                entry.delegation_blob_hash
            )
    raise DelegationNotFoundError(
        "no active delegation found for delegate"
    )
```

### list_delegates() Implementation Pattern
```python
# Source: derived from CONTEXT.md D-03 and existing patterns
async def list_delegates(self) -> list[DelegationEntry]:
    """List active delegates in the directory namespace.

    Returns:
        List of DelegationEntry objects.

    Raises:
        DirectoryError: If not in admin mode.
        ProtocolError: If the node rejects the request.
        ConnectionError: If the request times out.
    """
    if not self._is_admin:
        raise DirectoryError("only admin can list delegates")
    result = await self._client.delegation_list(
        self._directory_namespace
    )
    return result.entries
```

### DelegationNotFoundError Exception
```python
# Source: derived from CONTEXT.md D-04, follows existing hierarchy in exceptions.py
class DelegationNotFoundError(DirectoryError):
    """No active delegation found for the specified delegate."""
```

### Unit Test Pattern for revoke_delegation
```python
# Source: derived from existing test_directory.py patterns (lines 301-327)
class TestRevokeDelegation:
    async def test_revoke_success(self, identity: Identity) -> None:
        delegate = Identity.generate()
        pk_hash = sha3_256(delegate.public_key)
        blob_hash = b"\xdd" * 32

        client = make_mock_client(identity)
        client.delegation_list = AsyncMock(
            return_value=DelegationList(
                entries=[DelegationEntry(
                    delegate_pk_hash=pk_hash,
                    delegation_blob_hash=blob_hash,
                )]
            )
        )
        dr = DeleteResult(tombstone_hash=b"\xee" * 32, seq_num=5, duplicate=False)
        client.delete_blob = AsyncMock(return_value=dr)

        d = Directory(client, identity)
        result = await d.revoke_delegation(delegate)

        assert result is dr
        client.delegation_list.assert_called_once_with(identity.namespace)
        client.delete_blob.assert_called_once_with(blob_hash)
```

### Integration Test Pattern for Revocation Propagation
```python
# Source: derived from test_integration.py pattern (lines 56-68) + test_acl04_revocation.sh flow
@pytest.mark.integration
async def test_delegation_revocation_propagation() -> None:
    """REV-02: revoked delegate writes rejected after tombstone propagation."""
    owner = Identity.generate()
    delegate = Identity.generate()

    async with ChromatinClient.connect([(RELAY_HOST, RELAY_PORT)], owner) as owner_conn:
        directory = Directory(owner_conn, owner)
        # 1. Delegate
        await directory.delegate(delegate)
        # 2. Delegate writes (should succeed)
        async with ChromatinClient.connect([(RELAY_HOST, RELAY_PORT)], delegate) as del_conn:
            del_conn_dir = Directory(del_conn, delegate, directory_namespace=owner.namespace)
            # delegate writes blob to owner's namespace
            await del_conn.write_blob(b"test-data", ttl=300)
        # 3. Revoke
        result = await directory.revoke_delegation(delegate)
        assert isinstance(result, DeleteResult)
        # 4. Wait for propagation
        await asyncio.sleep(5)
        # 5. Delegate write should fail
        async with ChromatinClient.connect([(RELAY_HOST, RELAY_PORT)], delegate) as del_conn2:
            # Attempt write to owner's namespace -- expect rejection
            # ... (exact error handling depends on how node rejection is surfaced)
```

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | pytest 9.0.2 + pytest-asyncio |
| Config file | `sdk/python/pyproject.toml` [tool.pytest.ini_options] |
| Quick run command | `cd sdk/python && .venv/bin/python -m pytest tests/test_directory.py -x -v` |
| Full suite command | `cd sdk/python && .venv/bin/python -m pytest tests/ -x -v --ignore=tests/test_integration.py` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| REV-01 | revoke_delegation() tombstones delegation blob | unit | `cd sdk/python && .venv/bin/python -m pytest tests/test_directory.py::TestRevokeDelegation -x` | Wave 0 (new class) |
| REV-01 | list_delegates() returns active delegates | unit | `cd sdk/python && .venv/bin/python -m pytest tests/test_directory.py::TestListDelegates -x` | Wave 0 (new class) |
| REV-01 | DelegationNotFoundError raised for non-existent delegation | unit | `cd sdk/python && .venv/bin/python -m pytest tests/test_directory.py::TestRevokeDelegation::test_revoke_not_found -x` | Wave 0 (new test) |
| REV-01 | Admin check on revoke_delegation() | unit | `cd sdk/python && .venv/bin/python -m pytest tests/test_directory.py::TestRevokeDelegation::test_revoke_non_admin_raises -x` | Wave 0 (new test) |
| REV-02 | Revoked delegate writes rejected after sync propagation | integration | `cd sdk/python && .venv/bin/python -m pytest tests/test_integration.py::test_delegation_revocation_propagation -x -v -m integration` | Wave 0 (new test) |

### Sampling Rate
- **Per task commit:** `cd sdk/python && .venv/bin/python -m pytest tests/test_directory.py -x -v`
- **Per wave merge:** `cd sdk/python && .venv/bin/python -m pytest tests/ -x -v --ignore=tests/test_integration.py`
- **Phase gate:** Full suite green + integration test green before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] `tests/test_directory.py::TestRevokeDelegation` -- covers REV-01 revocation success, not-found, non-admin
- [ ] `tests/test_directory.py::TestListDelegates` -- covers REV-01 list delegates success, non-admin, empty list
- [ ] `tests/test_integration.py::test_delegation_revocation_propagation` -- covers REV-02 multi-node propagation
- [ ] `make_mock_client()` update -- add `delegation_list = AsyncMock()` and `delete_blob = AsyncMock()`

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| C++ loadgen-driven revocation test | SDK-driven revocation via Python API | Phase 91 (now) | Users get a clean `revoke_delegation()` method instead of manual tombstone construction |
| No SDK revocation surface | delegate/revoke/list triad on Directory | Phase 91 (now) | Completes delegation lifecycle management in SDK |

## Open Questions

1. **Integration test: how does the node signal write rejection to the SDK?**
   - What we know: The C++ node rejects writes from revoked delegates with "no ownership or delegation" in logs. The loadgen-based test checks node logs.
   - What's unclear: What wire-level response does the SDK receive? Likely a ProtocolError from `write_blob()`. Need to verify the exact response type returned by the node for a rejected write (probably a non-DataAck response type).
   - Recommendation: The integration test should `pytest.raises(ProtocolError)` on the delegate's write attempt after revocation. If the exact error type differs, adjust in implementation.

2. **Should `make_mock_client()` be updated globally or per-test?**
   - What we know: The current `make_mock_client()` has 4 AsyncMock methods. Adding 2 more affects 81 existing tests.
   - What's unclear: Whether existing tests care about extra mock attributes (they shouldn't -- MagicMock ignores unused attributes).
   - Recommendation: Update `make_mock_client()` to include `delegation_list` and `delete_blob` as AsyncMock. This is safe because AsyncMock attributes on a MagicMock are inert if not called. Keeps test helper comprehensive.

## Sources

### Primary (HIGH confidence -- source code verified)
- `sdk/python/chromatindb/_directory.py` -- Directory class, delegate() method (line 330), admin guard pattern
- `sdk/python/chromatindb/client.py` -- delete_blob() (line 514), delegation_list() (line 865)
- `sdk/python/chromatindb/types.py` -- DelegationEntry (line 165), DelegationList (line 173), DeleteResult (line 28)
- `sdk/python/chromatindb/exceptions.py` -- full exception hierarchy, DirectoryError (line 94)
- `sdk/python/chromatindb/__init__.py` -- __all__ exports (line 76)
- `sdk/python/tests/test_directory.py` -- make_mock_client() (line 236), TestDelegate (line 298)
- `sdk/python/tests/test_client_ops.py` -- TestDelegationList (line 1435)
- `sdk/python/tests/test_integration.py` -- integration test patterns, relay skip guard (line 56)
- `sdk/python/tests/conftest.py` -- shared fixtures
- `sdk/python/pyproject.toml` -- pytest config (line 73)

### Secondary (HIGH confidence -- project research docs)
- `.planning/research/SUMMARY.md` -- Architecture approach, feature breakdown
- `.planning/research/PITFALLS.md` -- Pitfalls 1 (propagation window), 2 (tombstone ordering race), 5 (TTL=0 permanence)
- `.planning/milestones/v1.0.0-phases/48-access-control-topology/48-VERIFICATION.md` -- Tombstone-based revocation proven in Docker tests
- `tests/integration/test_acl04_revocation.sh` -- Docker integration test reference for revocation flow

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- no new dependencies, all existing libraries verified in sdk/python/.venv
- Architecture: HIGH -- all four modified files inspected, existing patterns documented from source
- Pitfalls: HIGH -- grounded in specific code locations, verified against existing test infrastructure
- Testing: HIGH -- pytest 9.0.2 verified working, test collection confirmed (81 tests in test_directory.py)

**Research date:** 2026-04-06
**Valid until:** 2026-05-06 (stable -- no external dependency changes expected)
