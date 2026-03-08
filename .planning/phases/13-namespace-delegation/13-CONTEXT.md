# Phase 13: Namespace Delegation - Context

**Gathered:** 2026-03-08
**Status:** Ready for planning

<domain>
## Phase Boundary

Namespace owners can grant write access to other pubkeys via signed delegation blobs, enabling multi-writer namespaces. Delegates sign with their own key; the node verifies delegation before accepting writes. Revocation via tombstoning the delegation blob. Delegates are write-only (no delete). No delegation chains, no configurable permissions, no delegation listing query.

</domain>

<decisions>
## Implementation Decisions

### Delegation lifetime
- Delegation blobs are permanent (TTL=0), like tombstones — no auto-expiry
- Owner must explicitly tombstone the delegation blob to revoke
- No limit on number of delegates per namespace (app layer can impose limits)
- Content-addressed dedup handles duplicate delegation blobs naturally (same data = same hash = deduped)

### Revocation semantics
- Delegate's existing blobs remain in the namespace after revocation — no cascade delete
- Owner can individually tombstone specific delegate-written blobs if needed
- Real-time delegation check: every delegate write verifies delegation blob exists AND no tombstone for it
- Reject writes from revoked delegates immediately, even when arriving via sync
- Re-delegation allowed: new delegation blob for same pubkey gets a new timestamp → new content hash → not blocked by old tombstone
- Blobs survive revocation cycles — once written and verified, they persist independently of delegation status

### Delegate attribution
- Delegate's pubkey in the blob IS the attribution — no extra fields or flags
- Readers can derive SHA3(pubkey) to see it doesn't match the namespace → delegate write
- Delegate writes into the owner's namespace (same namespace_id = SHA3(owner_pubkey))
- Delegation blob data = magic prefix + raw delegate pubkey only (no labels/metadata — app layer concern)
- Same canonical signing form: SHA3(namespace || data || ttl || timestamp), signed by delegate's key
- Engine verification: (1) signature valid for pubkey, (2) delegation blob exists mapping this pubkey to this namespace

### Claude's Discretion
- Delegation magic prefix value (following tombstone pattern)
- Delegation index implementation for efficient hot-path lookup (success criteria requires indexed, not scan)
- Internal storage layout for delegation index
- Error messages and logging for delegation-related rejections

</decisions>

<specifics>
## Specific Ideas

- Delegation blob follows the tombstone pattern: magic prefix + payload (delegate pubkey)
- Verification path in `BlobEngine::ingest()`: ownership check OR valid delegation check (fail-fast order preserved)
- Delegation blobs replicate via sync like any other blob (DELEG-03 is free — no special sync logic)

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `wire::TOMBSTONE_MAGIC` / `wire::is_tombstone()` / `wire::make_tombstone_data()`: Pattern for magic-prefixed data blobs — delegation can follow identical pattern
- `BlobEngine::ingest()` (`db/engine/engine.cpp:40`): Ownership check at step 2 (`SHA3(pubkey) == namespace_id`) — needs delegation bypass path
- `BlobEngine::delete_blob()` (`db/engine/engine.cpp:134`): Tombstone infrastructure for revocation — already works
- `wire::build_signing_input()` (`db/wire/codec.h:30`): Canonical signing — delegates reuse same form

### Established Patterns
- Magic prefix pattern: 4-byte prefix identifies blob type (tombstones use `0xDEADBEEF`)
- Fail-fast validation: structural → namespace → signature (cheapest to most expensive)
- Content-addressed dedup via SHA3-256 hash in storage layer
- TTL=0 for permanent blobs (tombstones, now delegations)

### Integration Points
- `BlobEngine::ingest()`: Add delegation verification as alternative to ownership check
- `BlobEngine::delete_blob()`: Must remain owner-only (no delegation bypass)
- `Storage`: May need new index/sub-database for delegation lookup (hot-path performance)
- Sync protocol: No changes needed — delegation blobs are regular blobs

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 13-namespace-delegation*
*Context gathered: 2026-03-08*
