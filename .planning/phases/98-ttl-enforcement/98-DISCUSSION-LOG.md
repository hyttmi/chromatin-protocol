# Phase 98: TTL Enforcement - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md -- this log preserves the alternatives considered.

**Date:** 2026-04-08
**Phase:** 98-ttl-enforcement
**Areas discussed:** Expiry check placement, Overflow semantics, Stats & Exists filtering, BlobFetch rejection, List/TimeRange filtering cost, BatchExists coverage, Storage expiry_map overflow, Sync hash collection, SyncProtocol::is_blob_expired migration, Sync blob send path, BlobNotify for near-expiry blobs, Test strategy, Client Notification suppression, Sync hash collection cost, saturating_expiry location, BatchRead truncation semantics, Scope confirmation, get_blobs_by_seq filtering, is_blob_expired uses saturating_expiry, NamespaceStats query, Expired-query logging, Metrics counter, Tombstone expiry, TimeRange handler, BlobNotify has_blob false positive, SyncOrchestrator get_blob calls, Engine::ingest already-expired blobs, PROTOCOL.md documentation, IngestError enum, Existing TTL>0 tombstone test, Delegation query expiry, Time snapshot in multi-blob handlers

---

## Expiry Check Placement

| Option | Description | Selected |
|--------|-------------|----------|
| Handler-level checks | Each handler calls shared is_blob_expired() after fetching. Explicit, easy to audit. | ✓ |
| Storage-level filtering | Modify get_blob/has_blob to accept timestamp and filter internally. | |
| Engine-level wrapper | Add expiry-aware wrappers on Engine. | |

**User's choice:** Handler-level checks
**Notes:** Explicit checks preferred over changing stable storage/engine APIs.

---

## is_blob_expired Location

| Option | Description | Selected |
|--------|-------------|----------|
| Free function in db/wire/codec.h | Lives next to BlobData struct. All handlers already include codec.h. | ✓ |
| Free function in db/util/blob_helpers.h | Groups with other blob utilities. | |
| You decide | Claude picks. | |

**User's choice:** codec.h

---

## Clock Source

| Option | Description | Selected |
|--------|-------------|----------|
| std::time(nullptr) inline | Simple, no plumbing. Tests use far-future timestamps. | ✓ |
| Injected clock reference | More testable but requires constructor changes. | |
| You decide | | |

**User's choice:** std::time(nullptr) inline

---

## Overflow Semantics

| Option | Description | Selected |
|--------|-------------|----------|
| Clamp to UINT64_MAX = never expires | Saturating add. Writer wanted a long-lived blob. | ✓ |
| Treat as already expired | Defensive, rejects pathological input. | |
| Reject at ingest | Prevents overflow data from entering storage. | |

**User's choice:** Clamp to UINT64_MAX

---

## ExistsRequest Filtering

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, filter expired | Upgrade has_blob to get_blob + expiry check. | ✓ |
| No, report storage truth | Exists reflects physical storage. | |

**User's choice:** Filter expired

---

## StatsRequest Filtering

| Option | Description | Selected |
|--------|-------------|----------|
| No, report storage reality | Stats shows actual storage usage. Filtering would be O(n). | ✓ |
| Yes, filter expired from counts | Client sees only live blob counts. | |
| You decide | | |

**User's choice:** Storage reality

---

## BlobFetch Rejection

| Option | Description | Selected |
|--------|-------------|----------|
| Return not-found 0x01 | Simple, no protocol changes. | ✓ |
| New status 0x02 = expired | More informative but adds wire status. | |

**User's choice:** Not-found 0x01

---

## List/TimeRange Filtering Cost

| Option | Description | Selected |
|--------|-------------|----------|
| Fetch full blob per ref to check | Max 100 lookups (list cap). Correctness over performance. | ✓ |
| Accept stale results between scans | Don't filter List results. | |
| You decide | | |

**User's choice:** Fetch full blob per ref

---

## BatchExists Coverage

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, filter for consistency | Same logic as single ExistsRequest. | ✓ |
| No, stay within TTL-02 scope | Only fix named handlers. | |

**User's choice:** Filter for consistency

---

## Storage expiry_map Overflow

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, fix everywhere | Use saturating_expiry in storage.cpp too. | ✓ |
| Out of scope for TTL-03 | Internal correctness fix, not serving path. | |

**User's choice:** Fix everywhere

---

## Sync Hash Collection

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, filter in hash collection too | Belt-and-suspenders. | ✓ |
| No, leave sync as-is | Expired blobs in hash lists are harmless. | |

**User's choice:** Filter in hash collection too

---

## SyncProtocol::is_blob_expired Migration

| Option | Description | Selected |
|--------|-------------|----------|
| Delete and redirect | Remove entirely, all callers use codec.h version. | ✓ |
| Keep as forwarding wrapper | Avoids touching sync test files. | |

**User's choice:** Delete and redirect

---

## Sync Blob Send Path

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, filter before sending | Prevents sending blobs that expire between Phase A and C. | ✓ |
| No, send whatever is requested | Hash collection already filters. | |

**User's choice:** Filter before sending

---

## BlobNotify for Near-Expiry Blobs

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, suppress if expired | Step 0 pattern before fan-out. | ✓ |
| No, let BlobFetch rejection handle it | Notify is harmless. | |
| You decide | | |

**User's choice:** Suppress if expired

---

## Test Strategy

| Option | Description | Selected |
|--------|-------------|----------|
| Past timestamp + short TTL | timestamp = now - 1000, ttl = 100. Always expired. | ✓ |
| TTL=0 vs TTL=1 with old timestamp | Simpler numbers. | |
| You decide | | |

**User's choice:** Past timestamp + short TTL

---

## Client Notification Suppression

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, suppress both | BlobNotify and Notification skip expired. | ✓ |
| No, notify clients anyway | Clients may want to know. | |

**User's choice:** Suppress both

---

## Sync Hash Collection Cost

| Option | Description | Selected |
|--------|-------------|----------|
| Per-hash get_blob + expiry check | Simple, correct, ~50-100x slower but acceptable at scale. | ✓ |
| Cross-reference expiry_map | Faster but adds new storage API. | |
| You decide | | |

**User's choice:** Per-hash get_blob + expiry check
**Notes:** User asked about performance impact. Analysis: ~50-100ms for 1,000 blobs (decrypt dominates). Acceptable for periodic sync.

---

## saturating_expiry Location

| Option | Description | Selected |
|--------|-------------|----------|
| In codec.h next to is_blob_expired | Both operate on BlobData semantics. | ✓ |
| In endian.h with checked_add/checked_mul | Arithmetic helper. | |
| You decide | | |

**User's choice:** codec.h

---

## BatchRead Truncation

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, treat as not-found (status 0x00) | Same as if blob didn't exist. Truncation flag unaffected. | ✓ |
| Skip entry entirely | Reduces response size but changes count semantics. | |

**User's choice:** Treat as not-found

---

## Scope Confirmation

| Option | Description | Selected |
|--------|-------------|----------|
| Keep expanded scope | Phase goal says "ANY code path." Extras are small. | ✓ |
| Strict TTL-01/02/03 only | Tighter scope, faster execution. | |
| Partial expansion | Keep some, defer others. | |

**User's choice:** Keep expanded scope

---

## get_blobs_by_seq Filtering

| Option | Description | Selected |
|--------|-------------|----------|
| No, leave as-is | Test-only API. Tests need to see all blobs. | ✓ |
| Yes, filter for completeness | Would break existing test logic. | |

**User's choice:** Leave as-is

---

## is_blob_expired Uses saturating_expiry

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, use saturating_expiry | Single source of truth. | ✓ |
| Keep raw addition | Separate overflow logic. | |

**User's choice:** Use saturating_expiry

---

## NamespaceStats Query

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, storage reality | Same as StatsRequest. No filtering. | ✓ |
| Filter expired from counts | Expensive per-blob scanning. | |

**User's choice:** Storage reality

---

## Expired-Query Logging

| Option | Description | Selected |
|--------|-------------|----------|
| Debug-level log | spdlog::debug. Zero overhead in production. | ✓ |
| No logging | Silent filtering. | |
| You decide | | |

**User's choice:** Debug-level log

---

## Metrics Counter

| Option | Description | Selected |
|--------|-------------|----------|
| No counter | Debug logging sufficient. Out of scope for hardening. | ✓ |
| Add counter | Useful but adds plumbing complexity. | |

**User's choice:** No counter

---

## Tombstone Expiry

| Option | Description | Selected |
|--------|-------------|----------|
| Reject TTL>0 tombstones at ingest | Tombstones must be permanent. New IngestError::invalid_ttl. | ✓ |
| Out of scope | Different concern from blob TTL. | |

**User's choice:** Reject TTL>0 tombstones
**Notes:** User clarified "tombstones have to be always ttl0 so they are permanent" and that this is NOT out of scope.

---

## TimeRange Handler

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, standard expiry check | Same pattern as all handlers. Zero extra cost. | ✓ |
| You decide | | |

**User's choice:** Standard expiry check

---

## BlobNotify has_blob False Positive

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, check expiry | Upgrade has_blob to get_blob + expiry. Prevents stale blocking. | ✓ |
| No, keep has_blob | Avoids full blob read on hot path. | |

**User's choice:** Check expiry

---

## SyncOrchestrator get_blob Calls

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, check expiry | Covers TOCTOU window. Zero extra cost. | ✓ |
| No, hash collection filter sufficient | TOCTOU window very narrow. | |

**User's choice:** Check expiry

---

## Engine::ingest Already-Expired Blobs

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, reject already-expired | Saves storage writes, scanner work, sync bandwidth. | ✓ |
| No, accept and let scanner clean | Keep ingest simple. | |
| You decide | | |

**User's choice:** Reject already-expired

---

## IngestError Enum for Tombstone TTL

| Option | Description | Selected |
|--------|-------------|----------|
| New invalid_ttl value | Distinct from malformed_blob and timestamp_rejected. | ✓ |
| Reuse malformed_blob | No new enum value. | |
| You decide | | |

**User's choice:** New invalid_ttl

---

## Existing TTL>0 Tombstone Test

| Option | Description | Selected |
|--------|-------------|----------|
| Add Engine-level rejection test too | Keep storage test AND add Engine test for TTL>0 rejection. | ✓ |
| Keep as storage-level test | Validates scanner handles edge case. | |
| Delete the test | Testing impossible state. | |

**User's choice:** Both tests

---

## Delegation Query Expiry

| Option | Description | Selected |
|--------|-------------|----------|
| Out of scope | Queries delegation_map index, not blob storage. Permanent by design. | ✓ |
| Check delegation blob expiry | Per-entry blob lookups. | |

**User's choice:** Out of scope

---

## Time Snapshot in Multi-Blob Handlers

| Option | Description | Selected |
|--------|-------------|----------|
| Once at handler start | Consistent within single response. | ✓ |
| Per-blob check | Slightly more correct but inconsistent within response. | |

**User's choice:** Once at handler start

---

## PROTOCOL.md Documentation

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, add TTL enforcement section | Document all TTL behavior. | ✓ |
| No, code is documentation | | |

**User's choice:** Full documentation update
**Notes:** User emphasized: "fix all the documentation after this, including the TTL enforcement section and make sure it's known by everyone that tombstone ttl is 0!"

---

## Claude's Discretion

- Exact `saturating_expiry` function signature details
- Where exactly in each handler the expiry check goes
- Plan decomposition (how many plans, ordering)
- Exact log message format
- Whether sync hash filtering uses `std::erase_if` or builds new vector

## Deferred Ideas

None -- expanded scope was explicitly confirmed by user.
