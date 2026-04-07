# Phase 93: Group Membership Revocation - Context

**Gathered:** 2026-04-07
**Status:** Ready for planning

<domain>
## Phase Boundary

Group admins can remove members so that future `write_to_group()` calls exclude removed members from the encrypted envelope's recipient stanzas. The removed member cannot decrypt new group data. Old data remains readable (no re-encryption). All SDK-only Python changes: zero C++ node modifications, zero new wire types.

</domain>

<decisions>
## Implementation Decisions

### Cache Refresh Strategy
- **D-01:** `write_to_group()` calls `directory.refresh()` before `directory.get_group()` — forces a full namespace rescan every time. Guarantees the freshest group membership view before encrypting.
- **D-02:** `refresh()` clears both user and group caches (existing behavior). One call covers group membership AND latest KEM public keys for remaining members via `resolve_recipient()`.

### Forward Exclusion Guarantee
- **D-03:** `write_to_group()` encrypts to whatever members the directory shows after refresh. If the removal blob hasn't propagated to the writer's node yet, the removed member is still included. This is consistent with the system's eventual consistency model — no special-casing for groups.
- **D-04:** Old data encrypted before removal remains readable by the removed member. No re-encryption. Forward exclusion only.

### Testing Strategy
- **D-05:** Unit tests in `test_directory.py` and `test_client_ops.py`: verify `refresh()` is called before `get_group()` in `write_to_group()`, verify removed member is absent from envelope recipients after refresh.
- **D-06:** KVM integration test against live 3-node swarm (192.168.1.200-202): admin removes member, writer refreshes and encrypts to group, verify removed member cannot decrypt the new group message. Same pattern as Phase 91 integration tests.

### Claude's Discretion
- Internal ordering of refresh + resolve within `write_to_group()`
- Test fixture design and mock structure
- Exact assertion patterns for proving excluded membership
- Whether to add a helper for the "encrypt then verify exclusion" test pattern

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Requirements
- `.planning/REQUIREMENTS.md` — GRP-01 (remove member, exclude from stanzas), GRP-02 (force cache refresh on group write)

### Existing Implementation (modify these)
- `sdk/python/chromatindb/client.py` — `write_to_group()` (line ~1031): add `directory.refresh()` call before `get_group()`
- `sdk/python/chromatindb/_directory.py` — `remove_member()` (line ~582): already exists, read-modify-write on group blob. `refresh()` (line ~645): already exists, clears all caches.

### Existing Implementation (read-only reference)
- `sdk/python/chromatindb/_directory.py` — `create_group()` (line ~522), `add_member()` (line ~548), `get_group()` (line ~629), `_populate_cache()` (line ~657)
- `sdk/python/chromatindb/_envelope.py` — `envelope_encrypt()`, `envelope_decrypt()` with key ring fallback (Phase 92)
- `sdk/python/chromatindb/identity.py` — `Identity` class with KEM key ring (Phase 92)

### Tests (extend these)
- `sdk/python/tests/test_directory.py` — Existing group tests. Add removal + exclusion tests.
- `sdk/python/tests/test_client_ops.py` — Existing `write_to_group` tests. Add refresh verification.
- `sdk/python/tests/test_envelope.py` — Existing envelope tests. Reference for recipient verification patterns.

### Prior Phase Context
- `.planning/phases/92-kem-key-versioning/92-CONTEXT.md` — Key ring and UserEntry v2 decisions (D-01 through D-11)
- `.planning/phases/91-sdk-delegation-revocation/91-CONTEXT.md` — KVM integration test pattern (D-06, D-07)

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `Directory.remove_member(group_name, member)` — Already implements the group membership removal (read-modify-write)
- `Directory.refresh()` — Already clears all caches (users + groups), forcing next query to rescan
- `client.write_to_group(data, group_name, directory, ttl)` — Existing group write, needs `refresh()` call added
- `envelope_encrypt(data, recipients, sender)` — Creates per-recipient KEM stanzas, already respects recipient list

### Established Patterns
- Directory methods check `self._is_admin` for admin-only operations
- `self._dirty = True` triggers repopulation on next query, but `refresh()` is the explicit full-clear mechanism
- `write_to_group()` resolves `Identity` objects from member hashes via `get_user_by_pubkey()`
- Groups stored as blobs with `TTL=0` (permanent) in the admin's namespace

### Integration Points
- `client.py` — `write_to_group()`: add `directory.refresh()` before `directory.get_group()`
- `test_directory.py` — New tests for removal + exclusion verification
- `test_client_ops.py` — New tests for `write_to_group()` refresh behavior
- KVM integration test script (new file)

</code_context>

<specifics>
## Specific Ideas

- The core change is a single `directory.refresh()` call at the top of `write_to_group()` before the existing `directory.get_group()` call
- KVM integration test flow: (1) admin creates group with 3 members, (2) writer encrypts to group — all 3 can decrypt, (3) admin removes member, (4) writer encrypts to group again — only 2 can decrypt, removed member gets `NotARecipientError`
- Phase 91 KVM integration test is the template for the new integration test

</specifics>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope

</deferred>

---

*Phase: 93-group-membership-revocation*
*Context gathered: 2026-04-07*
