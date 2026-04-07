# Phase 93: Group Membership Revocation - Research

**Researched:** 2026-04-07
**Domain:** SDK Python -- group membership, directory cache, envelope encryption
**Confidence:** HIGH

## Summary

Phase 93 is a focused, well-scoped SDK-only change. The core modification is a single `directory.refresh()` call inserted at the top of `write_to_group()` before the existing `directory.get_group()` call. This guarantees that any recently removed members are excluded from the recipient list before envelope encryption occurs. The `remove_member()` method already exists and works correctly. The envelope encryption system already respects whatever recipient list it receives. The gap is solely that `write_to_group()` does not force a cache refresh, so stale cached membership could include a removed member.

The testing dimension has two layers: (1) unit tests proving `refresh()` is called and the removed member is absent from recipients, and (2) a KVM integration test against the live 3-node swarm following the Phase 91 test pattern.

**Primary recommendation:** Add `directory.refresh()` as the first line of `write_to_group()`, write unit tests for the refresh behavior and exclusion verification, and add a KVM integration test following the Phase 91 delegation revocation test pattern.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** `write_to_group()` calls `directory.refresh()` before `directory.get_group()` -- forces a full namespace rescan every time. Guarantees the freshest group membership view before encrypting.
- **D-02:** `refresh()` clears both user and group caches (existing behavior). One call covers group membership AND latest KEM public keys for remaining members via `resolve_recipient()`.
- **D-03:** `write_to_group()` encrypts to whatever members the directory shows after refresh. If the removal blob hasn't propagated to the writer's node yet, the removed member is still included. This is consistent with the system's eventual consistency model -- no special-casing for groups.
- **D-04:** Old data encrypted before removal remains readable by the removed member. No re-encryption. Forward exclusion only.
- **D-05:** Unit tests in `test_directory.py` and `test_client_ops.py`: verify `refresh()` is called before `get_group()` in `write_to_group()`, verify removed member is absent from envelope recipients after refresh.
- **D-06:** KVM integration test against live 3-node swarm (192.168.1.200-202): admin removes member, writer refreshes and encrypts to group, verify removed member cannot decrypt the new group message. Same pattern as Phase 91 integration tests.

### Claude's Discretion
- Internal ordering of refresh + resolve within `write_to_group()`
- Test fixture design and mock structure
- Exact assertion patterns for proving excluded membership
- Whether to add a helper for the "encrypt then verify exclusion" test pattern

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| GRP-01 | Owner can remove a member from a group; future `write_to_group()` excludes removed member from recipient stanzas | `remove_member()` already exists at `_directory.py:582`. The missing piece is the `refresh()` call in `write_to_group()` to pick up the removal. Envelope encryption already respects the recipient list it receives. |
| GRP-02 | Group write forces directory cache refresh to ensure removed member's exclusion is reflected | `refresh()` already exists at `_directory.py:645`. It clears `_cache`, `_by_name`, `_by_pubkey_hash`, `_groups`, and sets `_dirty = False`. The next `get_group()` call triggers `_populate_cache()` which does a full namespace rescan. |
</phase_requirements>

## Standard Stack

No new libraries needed. This phase modifies existing Python SDK code only.

### Core (existing)
| Library | Purpose | Relevant API |
|---------|---------|-------------|
| chromatindb SDK | Client + Directory + Envelope | `write_to_group()`, `refresh()`, `remove_member()`, `envelope_encrypt()` |
| pytest | Unit + integration testing | `pytest.mark.integration`, `asyncio_mode = "auto"` |
| unittest.mock | Client mocking for unit tests | `AsyncMock`, `MagicMock`, `patch` |

### No New Dependencies
Zero new packages. Zero pip installs.

## Architecture Patterns

### Existing Code to Modify

**1. `client.py` -- `write_to_group()` (line 1031)**

Current implementation:
```python
async def write_to_group(self, data, group_name, directory, ttl):
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

Required change (per D-01): Insert `directory.refresh()` before `directory.get_group()`.

```python
async def write_to_group(self, data, group_name, directory, ttl):
    directory.refresh()  # GRP-02: force cache refresh before group resolution
    group = await directory.get_group(group_name)
    # ... rest unchanged
```

Note: `refresh()` is synchronous (not async) -- it just clears dictionaries. The async cost is deferred to `get_group()` which triggers `_populate_cache()`.

### Existing Code (read-only reference)

**2. `_directory.py` -- `remove_member()` (line 582)**
Already implements read-modify-write: reads current group, filters out member hash, writes new GRPE blob. Sets `self._dirty = True`.

**3. `_directory.py` -- `refresh()` (line 645)**
Synchronous method that clears all caches:
```python
def refresh(self) -> None:
    self._cache = None
    self._by_name.clear()
    self._by_pubkey_hash.clear()
    self._groups.clear()
    self._dirty = False
```

**4. `_envelope.py` -- `envelope_encrypt()`**
Creates per-recipient KEM stanzas. If a member is not in the recipient list, no stanza is created for them, and they cannot decrypt.

### Data Flow for Group Membership Revocation

```
1. Admin calls directory.remove_member("team", removed_identity)
   -> reads current group blob, filters out member hash, writes new GRPE blob
   -> sets _dirty = True

2. New GRPE blob propagates to other nodes via sync/BlobNotify

3. Writer calls client.write_to_group(data, "team", directory, ttl)
   -> directory.refresh() clears all caches (GRP-02)
   -> directory.get_group("team") triggers _populate_cache()
   -> _populate_cache() scans namespace, finds latest GRPE blob (highest timestamp)
   -> latest GRPE blob has removed member absent
   -> envelope_encrypt creates stanzas only for remaining members
   -> removed member cannot find matching stanza -> NotARecipientError
```

### Anti-Patterns to Avoid
- **Using `_dirty = True` instead of `refresh()`:** Setting `_dirty` triggers repopulation but does NOT clear old cached data first. `refresh()` is the explicit full-clear mechanism per D-01.
- **Making `refresh()` async:** It is currently sync (just clears dicts). No reason to change the signature.
- **Adding propagation delay handling:** Per D-03, the system is eventually consistent. If the removal blob hasn't propagated, the removed member is still included. No special-casing.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Cache invalidation | Custom staleness tracking | `directory.refresh()` | Already exists, clears all caches atomically |
| Member exclusion | Custom recipient filtering | `remove_member()` + `_populate_cache()` | Group blob read-modify-write already proven |
| Recipient verification in tests | Manual stanza parsing | `envelope_decrypt()` with `NotARecipientError` | Existing exception is the definitive proof of exclusion |

## Common Pitfalls

### Pitfall 1: Forgetting refresh() is synchronous
**What goes wrong:** Writing `await directory.refresh()` causes a TypeError since `refresh()` returns `None`, not a coroutine.
**Why it happens:** Most directory methods are async.
**How to avoid:** Call `directory.refresh()` without `await`. It just clears dictionaries.
**Warning signs:** `TypeError: object NoneType can't be used in 'await' expression`

### Pitfall 2: Testing against wrong cache state
**What goes wrong:** Unit test passes but doesn't actually verify refresh was called -- the mock directory might already return the post-removal membership without needing a refresh.
**Why it happens:** Mock setup doesn't differentiate between pre-refresh and post-refresh state.
**How to avoid:** Mock `get_group()` to return different results based on whether `refresh()` was called. Use `side_effect` or track call order.
**Warning signs:** Test passes even when you comment out the `refresh()` call.

### Pitfall 3: _populate_cache keeps highest-timestamp group blob
**What goes wrong:** If an old group blob with the removed member has a higher timestamp than the new blob without them, the wrong membership is used.
**Why it happens:** `_populate_cache` uses timestamp comparison to resolve duplicate group names.
**How to avoid:** This is not actually a problem in practice -- `remove_member()` writes a new blob with the current timestamp, which will always be higher. Just be aware of the mechanism.
**Warning signs:** Stale membership after removal (only in pathological clock-skew scenarios).

### Pitfall 4: KVM integration test timing
**What goes wrong:** Test fails intermittently because the removal blob hasn't propagated to the writer's node.
**Why it happens:** Eventual consistency -- sync may take a few seconds in the LAN swarm.
**How to avoid:** Add a propagation delay (3-5 seconds) between `remove_member()` and the writer's `write_to_group()`, matching Phase 91's 5-second wait pattern.
**Warning signs:** Test passes locally but fails on CI or slower networks.

## Code Examples

### Example 1: Modified write_to_group()
```python
# Source: client.py line 1031 (modified per D-01, GRP-02)
async def write_to_group(
    self,
    data: bytes,
    group_name: str,
    directory: Directory,
    ttl: int,
) -> WriteResult:
    """Encrypt data for all group members and write as a blob.

    Forces a directory cache refresh before resolving group membership
    to ensure recently removed members are excluded (GRP-02).
    """
    directory.refresh()  # GRP-02: force cache refresh
    group = await directory.get_group(group_name)
    if group is None:
        raise DirectoryError(f"Group not found: {group_name}")
    recipients: list[Identity] = []
    for member_hash in group.members:
        entry = await directory.get_user_by_pubkey(member_hash)
        if entry is not None:
            recipients.append(entry.identity)
    return await self.write_encrypted(data, ttl, recipients)
```

### Example 2: Unit test -- refresh called before get_group
```python
# Pattern for test_client_ops.py
async def test_write_to_group_calls_refresh(client, mock_transport):
    """write_to_group calls directory.refresh() before get_group() (GRP-02)."""
    directory = MagicMock(spec=Directory)
    directory.refresh = MagicMock()
    directory.get_group = AsyncMock(return_value=GroupEntry(...))
    directory.get_user_by_pubkey = AsyncMock(return_value=None)

    # Track call order
    call_order = []
    directory.refresh.side_effect = lambda: call_order.append("refresh")
    original_get_group = directory.get_group.side_effect
    directory.get_group.side_effect = lambda *a, **kw: (
        call_order.append("get_group"), original_get_group(*a, **kw)
    )[-1]

    await client.write_to_group(b"data", "team", directory, 3600)
    assert call_order[0] == "refresh"
    assert call_order[1] == "get_group"
```

### Example 3: Unit test -- removed member excluded from recipients
```python
# Pattern for test_directory.py or test_client_ops.py
async def test_write_to_group_excludes_removed_member():
    """After remove_member, write_to_group encrypts only to remaining members."""
    # Setup: mock directory returns group WITHOUT removed member after refresh
    # Call write_to_group
    # Assert write_encrypted was called with recipients that exclude removed member
```

### Example 4: KVM integration test (following Phase 91 pattern)
```python
# Pattern from test_integration.py (Phase 91 delegation revocation test)
async def test_group_membership_revocation():
    """GRP-01 + GRP-02: admin removes member, writer excludes them."""
    admin = Identity.generate()
    writer = Identity.generate()
    member_a = Identity.generate()
    member_b = Identity.generate()  # will be removed

    async with ChromatinClient.connect(relays, admin) as admin_conn:
        directory = Directory(admin_conn, admin)
        # Register all identities, create group with [admin, writer, member_a, member_b]
        await directory.create_group("team", [admin, writer, member_a, member_b])

        # Writer encrypts to group -- all 4 can decrypt
        async with ChromatinClient.connect(relays, writer) as writer_conn:
            wr1 = await writer_conn.write_to_group(b"before removal", "team", directory, 300)

        # Admin removes member_b
        await directory.remove_member("team", member_b)
        await asyncio.sleep(5)  # propagation delay

        # Writer encrypts to group again -- only 3 can decrypt
        async with ChromatinClient.connect(relays, writer) as writer_conn:
            wr2 = await writer_conn.write_to_group(b"after removal", "team", directory, 300)

        # member_b cannot decrypt new message
        async with ChromatinClient.connect(relays, member_b) as mb_conn:
            result = await mb_conn.read_blob(admin.namespace, wr2.blob_hash)
            with pytest.raises(NotARecipientError):
                envelope_decrypt(result.data, member_b)
```

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | pytest 9.0.2 |
| Config file | `sdk/python/pyproject.toml` `[tool.pytest.ini_options]` |
| Quick run command | `cd sdk/python && .venv/bin/pytest tests/test_client_ops.py tests/test_directory.py -x -q` |
| Full suite command | `cd sdk/python && .venv/bin/pytest tests/ -x -q` |

### Phase Requirements to Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| GRP-01 | Removed member excluded from write_to_group recipients | unit | `.venv/bin/pytest tests/test_client_ops.py -x -k "write_to_group"` | No -- Wave 0 |
| GRP-01 | Removed member cannot decrypt new group data (E2E) | integration | `.venv/bin/pytest tests/test_integration.py -x -k "group_membership" -m integration` | No -- Wave 0 |
| GRP-02 | write_to_group forces directory.refresh() before get_group() | unit | `.venv/bin/pytest tests/test_client_ops.py -x -k "write_to_group_refresh"` | No -- Wave 0 |

### Sampling Rate
- **Per task commit:** `cd sdk/python && .venv/bin/pytest tests/test_client_ops.py tests/test_directory.py -x -q`
- **Per wave merge:** `cd sdk/python && .venv/bin/pytest tests/ -x -q`
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] `tests/test_client_ops.py` -- new test class `TestWriteToGroup` covering refresh behavior and recipient exclusion
- [ ] `tests/test_integration.py` -- new test `test_group_membership_revocation` following Phase 91 delegation revocation pattern
- No framework install needed -- pytest and asyncio fixtures already configured

## Open Questions

1. **Writer's directory instance sharing**
   - What we know: The KVM integration test has the writer create a `write_to_group` call with a directory object. In the current code, the writer needs a Directory instance connected to the admin's namespace to resolve group membership.
   - What's unclear: In the integration test, how does the writer get a Directory instance that reads the admin's namespace? The writer needs to construct `Directory(writer_conn, admin)` or use a separate directory connection.
   - Recommendation: The writer constructs a Directory with `admin_identity=admin` (public-only) pointing at the admin's namespace. The `refresh()` + `get_group()` pattern reads from that namespace. This is how existing group tests work -- the Directory constructor takes the admin identity to determine the directory namespace.

## Sources

### Primary (HIGH confidence)
- `sdk/python/chromatindb/client.py` lines 1031-1064 -- current `write_to_group()` implementation
- `sdk/python/chromatindb/_directory.py` lines 582-614 -- existing `remove_member()` implementation
- `sdk/python/chromatindb/_directory.py` lines 645-655 -- existing `refresh()` implementation
- `sdk/python/chromatindb/_directory.py` lines 657-746 -- `_populate_cache()` with group timestamp resolution
- `sdk/python/tests/test_integration.py` lines 475-527 -- Phase 91 KVM integration test pattern
- `sdk/python/tests/test_directory.py` lines 1293-1354 -- existing `remove_member` unit tests
- `sdk/python/tests/test_client_ops.py` lines 76-104 -- fixture pattern for mock transport/client
- `sdk/python/tests/test_envelope.py` line 119-126 -- `NotARecipientError` test pattern

### Secondary (MEDIUM confidence)
- `.planning/phases/93-group-membership-revocation/93-CONTEXT.md` -- user decisions and canonical refs

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- no new libraries, existing SDK code only
- Architecture: HIGH -- one-line code change (`refresh()` call), all supporting code exists
- Pitfalls: HIGH -- patterns are well-established from prior phases, KVM test timing is the only real risk

**Research date:** 2026-04-07
**Valid until:** 2026-05-07 (stable -- no external dependencies to change)
