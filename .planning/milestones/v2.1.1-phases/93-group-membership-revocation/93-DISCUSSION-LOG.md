# Phase 93: Group Membership Revocation - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-04-07
**Phase:** 93-group-membership-revocation
**Areas discussed:** Cache refresh strategy, Forward exclusion guarantee, Testing approach

---

## Cache Refresh Strategy

| Option | Description | Selected |
|--------|-------------|----------|
| Always refresh() before resolve | write_to_group() calls directory.refresh() then get_group(). Guarantees fresh data every time. One extra namespace scan per group write. | ✓ |
| Add force_refresh param | write_to_group(..., force_refresh=True) — default True, callers can opt out. More flexible, more API surface. | |
| Refresh only get_group() | Make get_group() always rescan (no caching for groups). Users and groups diverge in caching behavior. | |

**User's choice:** Always refresh() before resolve (Recommended)
**Notes:** Simple and correct. One call covers everything.

### Follow-up: User cache scope

| Option | Description | Selected |
|--------|-------------|----------|
| Single refresh() covers both | refresh() already clears both user and group caches. One call refreshes everything. | ✓ |
| Refresh groups only | Only invalidate group cache, leave user cache warm. Risks stale KEM keys. | |

**User's choice:** Single refresh() covers both (Recommended)
**Notes:** refresh() already clears everything — no changes needed to its behavior.

---

## Forward Exclusion Guarantee

| Option | Description | Selected |
|--------|-------------|----------|
| Encrypt with whatever is current | write_to_group() encrypts to whatever members the directory shows after refresh. Eventual consistency. | ✓ |
| Accept a deny-list parameter | write_to_group(..., exclude=[member]) lets caller explicitly exclude members. | |

**User's choice:** Encrypt with whatever is current (Recommended)
**Notes:** Consistent with the system's eventual consistency model everywhere. No special-casing.

---

## Testing Approach

| Option | Description | Selected |
|--------|-------------|----------|
| Unit tests only | Mock-based tests for refresh and exclusion logic. No integration. | |
| Unit + KVM integration | Unit tests plus live KVM swarm test for end-to-end flow. | ✓ |
| Unit + Docker integration | Unit tests plus Docker-based multi-node test. | |

**User's choice:** Unit + KVM integration
**Notes:** Same pattern as Phase 91 integration tests against 192.168.1.200-202 swarm.

---

## Claude's Discretion

- Internal ordering of refresh + resolve within write_to_group()
- Test fixture design and mock structure
- Exact assertion patterns for proving excluded membership

## Deferred Ideas

None — discussion stayed within phase scope
