# Phase 124: CLI Adaptation to New Protocol — Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-04-21
**Phase:** 124-cli-adaptation-to-new-mvp-protocol
**Areas discussed:** Auto-PUBK strategy, Delegate vs owner signer_hint routing, BlobData + call-site migration shape, Live-node E2E orchestration, Test migration scope

---

## Area selection

**Question:** Which implementation areas do you want to discuss for Phase 124?

| Option | Description | Selected |
|--------|-------------|----------|
| Auto-PUBK strategy | When/how cdb ensures a PUBK exists before first write | ✓ |
| Delegate vs owner signer_hint routing | Implicit vs explicit delegation identification | ✓ |
| BlobData + call-site migration shape | Helper vs wrapper vs pass-separately for 39 call sites | ✓ |
| Live-node E2E orchestration | Who runs E2E, what's in scope, gates | ✓ |

**User's choice:** All four.
**Notes:** User selected every gray area — full scope, no deferrals.

---

## Auto-PUBK strategy

**Question:** When should cdb emit the auto-PUBK?

| Option | Description | Selected |
|--------|-------------|----------|
| Per-session, on first write | Probe via ListByMagic(ns, PUBK) before first owner-write; publish if missing. One extra RTT + one PUBK per fresh namespace per process. | ✓ |
| Optimistic: retry on PUBK_FIRST_VIOLATION | Send user's blob directly; recover on error. Zero RTT for established namespaces; extra error round-trip for fresh ones. | |
| Unconditional PUBK at connect | Publish PUBK after every handshake. No probe, no state; wastes ~4.2 KB + one signature per invocation. | |
| Persistent cache (~/.chromatindb/pubk_cache.json) | Local file tracks PUBK'd (node, namespace) pairs. Zero cost on warm cache; stale-cache footguns on wipe. | |

**User's choice:** Per-session, on first write (Recommended).
**Notes:** Uses existing Phase 117 ListRequest + type_filter primitive — zero new node-side surface. In-process cache only, no persistence.

---

## Delegate vs owner signer_hint routing

**Question:** How should cdb identify delegate vs owner writes?

| Option | Description | Selected |
|--------|-------------|----------|
| Implicit: target_ns vs SHA3(own_pubkey) | cdb sets signer_hint = SHA3(own_signing_pubkey) regardless. Node routes via owner_pubkeys vs delegation_map. Zero flag surface. | ✓ |
| Explicit flag: --as <owner_ns_hex> | New flag on every write to target a non-own namespace. Explicit but redundant with --share @group. | |
| Config-driven: default_delegate_namespace | ~/.cdb/config.json field. Useful for service-account delegates. YAGNI for MVP. | |

**User's choice:** Implicit: compare target_ns vs SHA3(own_pubkey) (Recommended).
**Notes:** No new flags. Delegate vs owner is only a node-side concern (lookup-table routing). cdb stays flag-minimal.

---

## BlobData + call-site migration shape

**Question:** How should the BlobData migration look at call sites?

| Option | Description | Selected |
|--------|-------------|----------|
| Helper: build_owned_blob(id, ns, data, ttl) | Central helper returns (target_ns, BlobData). 39 sites collapse to one-liners. Respects no-duplicate-code. | ✓ |
| NamespacedBlob wrapper struct | Mirror db/sync struct; orthogonal to helper (could combine). | |
| Narrow BlobData + pass target_namespace separately | Mechanical rename; leaves signing sequence copy-pasted 39× unless paired with helper. | |

**User's choice:** Helper: build_owned_blob(id, ns, data, ttl) -> (target_ns, BlobData) (Recommended).
**Notes:** Helper returns NamespacedBlob struct (Claude's discretion; mirrors db/sync/sync_protocol.h vocabulary).

---

## Live-node E2E orchestration

**Question 1:** How do we run live-node E2E?

| Option | Description | Selected |
|--------|-------------|----------|
| I run E2E over SSH after you deploy node | User builds + deploys. Claude SSH to run matrix. Matches self-verify-checkpoints. | ✓ |
| I deliver E2E checklist, you run it | Script + expected output. Safer for prod data. Slower iteration. | |
| Local loopback E2E only | Post-124 node on localhost. Fails SC#7 literal (ROADMAP says 192.168.1.73). | |

**User's choice:** Custom response — "yep, i build to local and home (selectable on CLI with --node flag, check the .cdb dir in ~) so you test them and test the syncing and bombs and everything".
**Notes:** Stronger than any presented option — user deploys to BOTH local and home, Claude runs full E2E including multi-node sync and BOMB propagation. Configured via existing --node flag + ~/.cdb/ config.

**Question 2:** What's in-scope for the E2E matrix?

| Option | Description | Selected |
|--------|-------------|----------|
| SC#7 literal matrix | put → get → put --name → get <name> → rm → ls | ✓ |
| Plus chunked: >500 MiB file (CDAT/CPAR) | Validates Phase 119 under new envelope | ✓ |
| Plus delegate: cdb put --share @contact | Tests delegate-write path against node's delegation_map | ✓ |
| Plus --replace: cdb put --name foo --replace | Validates Phase 123 D-15 + BOMB-of-1 emission | ✓ |

**User's choice:** All four (plus multi-node sync implied by the custom E2E response).

---

## Closing / further exploration

**Question:** We've discussed auto-PUBK, delegate routing, migration shape, and E2E. Anything else before I write CONTEXT.md?

| Option | Description | Selected |
|--------|-------------|----------|
| I'm ready for context | Write CONTEXT.md, move on | |
| Explore test suite migration | SC#6 new coverage scope | ✓ |
| Explore error-surface wording | PUBK_FIRST_VIOLATION + PUBK_MISMATCH UX | |
| Explore config/node-switching | ~/.cdb/ + --node assumptions | |

**User's choice:** Explore test suite migration.

---

## Test migration scope (follow-up area)

**Claude re-scoped:** Earlier "13 legacy refs in cli/tests/" included 7 false positives from `test_identity.cpp` — those refer to `Identity::namespace_id()` (CLI-local method, unchanged post-122). Actual migration surface is tighter.

**Question:** What's the test scope for Phase 124?

| Option | Description | Selected |
|--------|-------------|----------|
| Surgical edits + new coverage | Update test_wire.cpp blob roundtrip in-place; add new TEST_CASEs for envelope, helper, NAME/BOMB roundtrips, auto-PUBK. test_chunked.cpp via helper. test_identity.cpp unchanged. | ✓ |
| Surgical + local integration test (fork a node in-test) | Everything above + spin up post-124 chromatindb locally for real put/get/rm integration tests. Overlaps with backlog 999.4. | |
| Minimal: only test_wire.cpp edits, defer coverage | Fails SC#6 ('new tests cover auto-PUBK, NAME, BOMB paths'). | |

**User's choice:** Surgical edits + new coverage (Recommended).

---

## Claude's Discretion

- Exact return type of `build_owned_blob` (NamespacedBlob struct recommended vs std::pair).
- In-process auto-PUBK cache location (Connection member vs free function vs new tiny module).
- Probe-failure recovery (proceed optimistically vs bail).
- Whether to delete MsgType::Delete = 17 from CLI enum after unification.
- Error-wording specifics within D-05 constraints.
- Test file split (new test_auto_pubk.cpp vs TEST_CASE in test_wire.cpp).
- D-06 cascade error-path semantics (fail whole rm vs partial BOMB + warning vs BOMB manifest only).
- Synchronous vs pipelined auto-PUBK probe (synchronous preferred for correctness).

## Deferred Ideas

- Persistent PUBK cache (~/.chromatindb/pubk_cache.json).
- Explicit delegate flag (--as, --delegate-for) or config field (default_delegate_namespace).
- Local-fork-a-node integration test harness (overlaps backlog 999.4).
- Pipelined probe + optimistic write optimization.
- MsgType::Delete = 17 removal (depends on unification outcome).
- cdb status / cdb info --pubk diagnostic.
- Shared error-code table header.
