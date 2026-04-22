---
phase: 125-docs-update-for-mvp-protocol
plan: 01
subsystem: docs
tags: [protocol, docs, wire-format, signer_hint, pubk-first, bomb, name, flatbuffers]

# Dependency graph
requires:
  - phase: 122-schema-signing-cleanup-strip-namespace-and-compress-pubkey
    provides: "signer_hint, BlobWriteBody, owner_pubkeys DBI, PUBK-first invariant"
  - phase: 123-tombstone-batching-and-name-tagged-overwrite
    provides: "NAME magic + resolver rules, BOMB magic + cascade, ttl=0 invariant"
  - phase: 124-cli-adaptation-to-new-mvp-protocol
    provides: "Error codes 0x07-0x0B with verbatim CLI wording from [error_decoder]"
provides:
  - "db/PROTOCOL.md anchors for Plan 2 (README) and Plan 3 (ARCHITECTURE.md) cross-links"
  - "Canonical external-client wire spec: post-122 5-field Blob, BlobWrite=64 envelope, Delete=17 retention, signer_hint semantics"
  - "Error-code table 0x01-0x0B with byte-identical CLI decoder wording"
  - "Ingest invariants documented at wire level: PUBK-first, BOMB ttl=0, single-target tombstone ttl=0"
  - "Sync Phase C per-blob target_namespace prefix contract"
  - "ListRequest type_filter extension-open documentation"
  - "request_id pipelining semantics with default depth (kPipelineDepth=8) noted"
affects: [125-02, 125-03, 125-04, 125-05]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Zero-duplication doc contract (D-02): PROTOCOL.md is the single source of byte-level truth; other docs cross-link by anchor"
    - "Code-as-ground-truth for doc content: MetadataResponse field semantic resolved by reading db/peer/message_dispatcher.cpp, not by guessing from old prose"
    - "ASCII-only diagrams (D-06): continues the pre-existing PROTOCOL.md style"

key-files:
  created:
    - .planning/phases/125-docs-update-for-mvp-protocol/125-01-SUMMARY.md
  modified:
    - db/PROTOCOL.md

key-decisions:
  - "MetadataResponse Type 48 emits 32-byte signer_hint (not resolved 2592-byte signing_pk); offline signature verification requires clients to fetch the namespace's PUBK blob and cache (signer_hint → signing_pk) locally"
  - "Data=8 row retained in Message Type Reference marked DELETED; Plan 04 will strip the enum slot after flatc regen"
  - "Tombstones and delegations documented as regular post-122 5-field Blobs wrapped in BlobWriteBody; Delete=17 retained only for ack-type differentiation"
  - "BOMB routing via BlobWrite=64 (not Delete=17) documented with explicit rationale tied to Delete dispatcher's 36-byte tombstone-format precondition"
  - "ListRequest blob_type index described as extension-open (any 4-byte magic classifiable without node change) — forward-facing design doc for new blob types"

patterns-established:
  - "Anchor set for Plan 3 cross-links: #blob-schema, #canonical-signing-input, #sending-a-blob-blobwrite--64, #namespace-derivation, #signer_hint-semantics, #owner_pubkeys-dbi, #pubk-blob-format, #pubk-first-invariant, #name-blob-format, #bomb-blob-format, #errorresponse-type-63, #phase-c-blob-transfer, #blob-deletion, #namespace-delegation, #metadatarequest-type-47--metadataresponse-type-48"
  - "Per-field source-of-truth citations (file:line) at the bottom of each new or rewritten section so future code changes surface as doc drift via grep"

requirements-completed:
  - DOCS-01
  - DOCS-02

# Metrics
duration: "interrupted+continuation"
completed: 2026-04-22
---

# Phase 125 Plan 01: db/PROTOCOL.md post-v4.1.0 Rewrite — Summary

**Rewrote db/PROTOCOL.md byte-for-byte against the post-122/123/124 codebase: 5-field Blob schema, BlobWriteBody envelope on BlobWrite=64, signer_hint + owner_pubkeys DBI, PUBK + PUBK-first, NAME + BOMB magics with ingest invariants, and error codes 0x07-0x0B with verbatim CLI decoder wording.**

## Performance

- **Duration:** ~10 h wall-clock across two executor runs (Tasks 1-4 in the 2026-04-21 20:15-20:19 UTC+3 run; Task 5 + SUMMARY in the 2026-04-22 05:30-05:57 UTC+3 continuation run after the prior executor hit its budget limit)
- **Started:** 2026-04-21T20:15:17+03:00 (first per-task commit)
- **Completed:** 2026-04-22T05:57:00+03:00 (approximate SUMMARY write time)
- **Tasks:** 5 (all autonomous, no checkpoints)
- **Files modified:** 1 (`db/PROTOCOL.md`), grew from 1386 → 1622 lines (+236 lines net)

## Accomplishments

- **db/PROTOCOL.md is now byte-level accurate against the post-phase-124 shipping binary.** An external client implementer can build a compliant writer against this doc + the `.fbs` schemas alone — no detail is deferred to `.planning/`.
- **Established all anchor targets for Plan 2 (README + cli/README.md) and Plan 3 (ARCHITECTURE.md)** to cross-link via without content duplication. Zero-duplication (D-02) is now structurally possible because PROTOCOL.md owns every byte-level fact.
- **Documented the five post-122 ingest invariants** at wire level: PUBK-first gate, BOMB ttl=0, BOMB owner-only (delegates rejected), BOMB structural-decode gate, and the Delete-dispatcher's 36-byte tombstone precondition (which forces BOMBs onto BlobWrite=64).
- **Resolved the open question on MetadataResponse** by direct code inspection: the node emits the 32-byte `signer_hint` directly, not the resolved 2592-byte signing pubkey (`db/peer/message_dispatcher.cpp:942,953,974-978`). Doc now reflects that and documents the PUBK-cache pattern for offline signature verification.
- **Error-code table 0x07-0x0B carries CLI wording byte-identical** to the `[error_decoder]` TEST_CASE literals in `cli/tests/test_wire.cpp`; any doc drift from now on breaks that test first.

## Task Commits

Each task was committed atomically against the main working tree:

1. **Task 1: Rewrite Blob schema, canonical signing, write-path sections** — `02926442` (docs) — 5-field Blob with `signer_hint`, `target_namespace` parameter rename, new §Sending a Blob (BlobWrite = 64) section; Data=8 marked DELETED in write-path prose.
2. **Task 2: Add namespace derivation, signer_hint, owner_pubkeys, PUBK, PUBK-first sections** — `68476ee3` (docs) — five new subsections under "## Storing a Blob" with byte-level PUBK blob format (4164 bytes total) and PUBK-first invariant covering direct writes, delegates, tombstones, BOMB, and sync Phase C.
3. **Task 3: NAME and BOMB blob format sections** — `59242147` (docs) — new "## Mutable Names and Batched Deletion" top-level section with both magics, byte layouts, resolver winner rule for NAME (timestamp DESC, blob_hash DESC tiebreak), D-15 same-second tiebreak quirk boxed as a Note, and BOMB invariants tied to error codes 0x09/0x0A/0x0B.
4. **Task 4: ErrorResponse extension with codes 0x07-0x0B** — `2524070e` (docs) — table extended to cover 0x01-0x0B, new "User-facing wording" column carries verbatim CLI literals; defensive decoder paths (short-read + unknown-code) documented; §Semantics line updated to `BlobWrite(64)` replacing `Data(8)`; cross-links to §PUBK-First and §BOMB.
5. **Task 5: Sync Phase C, Message Type Reference, MetadataResponse, tombstone/delegation, ListRequest/pipelining** — `8f496a99` (docs) — this run. Five reconciliation edits landed in one commit; residual `Data (8)` active-path mentions in Timestamp Validation, Rate Limiting, WriteAck, and ReadResponse also scrubbed.

**Plan metadata commit:** (pending — final commit after this SUMMARY write + STATE/ROADMAP updates.)

## Files Created/Modified

- `db/PROTOCOL.md` — Grew from 1386 to 1622 lines (+236 net). Post-122 Blob schema, canonical signing with `target_namespace` parameter, §Sending a Blob (BlobWrite = 64) with the `BlobWriteBody` envelope and write-path envelope-ownership table, five new storage-layer subsections (namespace derivation, signer_hint semantics, owner_pubkeys DBI, PUBK blob format, PUBK-first invariant), new "## Mutable Names and Batched Deletion" top-level section with NAME and BOMB magics + invariants, ErrorResponse table extended with 0x07-0x0B + defensive paths + user-facing wording column, Message Type Reference row 64 added for BlobWrite, Message Type Reference row 48 updated (pubkey → signer_hint), sync Phase C BlobTransfer wire format now includes per-blob 32-byte `target_namespace` prefix, MetadataResponse Type 48 rewrote Found-case table to reflect actual 32-byte signer_hint emission, Blob Deletion + Namespace Delegation sections rewritten against BlobWriteBody shape, ListRequest blob_type indexing note added, request_id pipelining default depth (kPipelineDepth=8) documented.
- `.planning/phases/125-docs-update-for-mvp-protocol/125-01-SUMMARY.md` (this file).

## Anchor List (for Plan 2 + Plan 3 cross-linking)

Plan 2 (README + cli/README.md) and Plan 3 (db/ARCHITECTURE.md) can link to these anchors without duplicating content:

| Anchor | Section |
|--------|---------|
| `#blob-schema` | Post-v4.1.0 5-field Blob FlatBuffer |
| `#canonical-signing-input` | `SHA3-256(target_namespace \|\| data \|\| ttl_be32 \|\| timestamp_be64)` |
| `#sending-a-blob-blobwrite--64` | BlobWrite=64 + BlobWriteBody envelope; Delete=17 vs BlobWrite=64 ownership table |
| `#namespace-derivation` | `namespace = SHA3-256(signing_pubkey)` + non-membership in Blob |
| `#signer_hint-semantics` | Owner vs delegate resolution rules |
| `#owner_pubkeys-dbi` | 8th DBI shape; self-healing rebuild on boot |
| `#pubk-blob-format` | 4164-byte layout + ingest side-effect (writes owner_pubkeys) |
| `#pubk-first-invariant` | Applies to direct, delegate, tombstone, BOMB, sync Phase C |
| `#name-blob-format` | NAME magic + resolver rules + D-15 tiebreak quirk |
| `#bomb-blob-format` | BOMB magic + ttl=0 + delegate-rejection + BlobWrite=64 routing |
| `#errorresponse-type-63` | Full 0x01-0x0B table + CLI wording + defensive paths |
| `#phase-c-blob-transfer` | Per-blob target_namespace prefix (Pitfall #3 resolution) |
| `#blob-deletion` | Single-target tombstone (Delete=17 + BlobWriteBody) |
| `#namespace-delegation` | Delegation blob (BlobWrite=64 + delegation DBI indexing) |
| `#metadatarequest-type-47--metadataresponse-type-48` | 32-byte signer_hint response + PUBK-cache pattern |
| `#listrequest--listresponse-types-33-34` | blob_type index extension-open note |

## Validation Grep Results (125-VALIDATION.md D1/D2)

**D1 presence greps (ALL PASS):**
- `grep -c "signer_hint" db/PROTOCOL.md` → 26 (≥ 3 required)
- `PUBK-first` + `PUBK_FIRST_VIOLATION` → present
- `BlobWrite` + `BlobWriteBody` → present
- `MsgType::Delete = 17` / `Delete (17)` → present
- `MsgType::BlobWrite = 64` / `BlobWrite (64)` → present
- `BOMB` + `NAME` + `CDAT` + `CPAR` → all present
- `target_namespace` → present
- `request_id` → present
- Error codes 0x07, 0x08, 0x09, 0x0A, 0x0B → all present

**D2 absence greps (ALL PASS):**
- `namespace_id: [ubyte]` → 0 matches (stale 6-field schema gone from Blob table definitions)
- `pubkey: [ubyte]` → 0 matches
- `Data (8)` as an active write path → 0 matches (the sole remaining mention is the DELETED row in the Message Type Reference)
- `MsgType::Data` → 0 matches

Note: legitimate `namespace_id` uses remain in wire-format tables for request/response payload offsets (e.g., `ReadRequest.namespace_id`) — these are storage/routing keys, not Blob fields, and are correctly preserved per the plan's absence-grep spec (only `BlobData.namespace_id` as a schema field is forbidden).

## Decisions Made

Decisions are enumerated in frontmatter `key-decisions`. Summary of the most load-bearing:

- **MetadataResponse emits `signer_hint`, not signing_pk.** Reading `db/peer/message_dispatcher.cpp:942,953,974-978` shows `signer_hint_len = 32` is hard-coded and `blob.signer_hint` is copied into the response. Documented with the rationale (2.5 KiB overhead per metadata response would dominate the payload) and the client-side mitigation (fetch PUBK once per namespace, cache locally).
- **Data=8 row retained (marked DELETED) rather than removed.** Plan 04's scope covers flatc regen and enum-slot removal; Plan 01 only marks the slot so external readers don't implement the wrong path. Consistent with `<task 5>` explicit instruction.
- **Tombstones and delegations are documented as regular post-122 5-field Blobs.** Internal `data` magic (`0xDEADBEEF` tombstone, `0xDE1E6A7E` delegation) distinguishes them. Delete=17 is kept solely for the distinct DeleteAck(18) ack type.
- **BOMB routing via BlobWrite=64 is the load-bearing correctness rule.** The Delete=17 dispatcher only accepts 36-byte tombstone-format `data`; BOMBs are `[BOMB:4][count:4BE][hash:32]×N` and fail that check. The doc now spells this out so external implementers don't reimplement the Phase 124 Plan 05 Rule-1 bug.
- **ListRequest blob_type index is extension-open.** No node-side schema change needed to classify a new 4-byte magic; clients just send the bytes they want. This frames the design as forward-facing.

## Deviations from Plan

None - plan executed exactly as written. Every edit in Tasks 1-5 corresponds to a specific `<action>` bullet in the PLAN.md spec; no additional code changes were required, no architectural deviations, no Rule 4 checkpoints encountered.

**Scope discipline:** Task 5 extended cleanup to include the four residual `Data (8)` active-path mentions found in §Timestamp Validation, §Rate Limiting, §WriteAck, and §ReadResponse. These were in-plan per the task's "Reconcile with post-122 context" framing and the VALIDATION.md D2 absence-grep requirement that no active-path `Data (8)` mentions remain. Treated as in-scope reconciliation, not deviation.

**Total deviations:** 0.
**Impact on plan:** Plan executed to spec.

## Issues Encountered

- **Budget interruption between Task 4 and Task 5.** The prior executor hit its budget limit after committing Tasks 1-4 and exited cleanly without creating the SUMMARY. This continuation run resumed from `2524070e` (master HEAD at continuation start), inspected `db/peer/message_dispatcher.cpp` for the MetadataResponse semantic, executed Task 5 as one atomic commit (`8f496a99`), and is now finalizing the SUMMARY + state. All Task 1-4 work is preserved in the git history; nothing was redone.
- **PreToolUse Read-before-edit reminders.** Each Edit on PROTOCOL.md surfaced a defensive READ-BEFORE-EDIT reminder even though the file had been Read multiple times earlier in the session (lines 32-111, 279-353, 540-774, 796-975, 975-1204, 1205-1404). Treated as a non-blocking hook; continued editing per the actual session-read state. No data loss.

## User Setup Required

None — documentation-only change.

## Next Phase Readiness

- **Plan 02 (README.md + cli/README.md) is unblocked.** All anchor targets in the PROTOCOL.md table above resolve; Plan 2 can produce the user-facing rewrite using cross-links without duplicating byte-level content.
- **Plan 03 (ARCHITECTURE.md)** has a clean PROTOCOL.md to link against. The code-as-ground-truth pattern established here (source-of-truth file:line citations at section footers) should carry into ARCHITECTURE.md for the same drift-detection benefit.
- **Plan 04 (Data=8 enum removal + D-11 flatc regen)** should update the Message Type Reference to delete row 8 entirely once the enum slot is gone from `transport.fbs` — the DELETED placeholder in row 8 is the documentation hook Plan 04 needs.
- **Plan 05 (comment hygiene pass)** unrelated to this plan's output; no cross-dependencies.

## Self-Check: PASSED

- `db/PROTOCOL.md` exists on disk.
- `.planning/phases/125-docs-update-for-mvp-protocol/125-01-SUMMARY.md` exists on disk.
- All five task commits (`02926442`, `68476ee3`, `59242147`, `2524070e`, `8f496a99`) verified present in `git log --oneline --all`.
- VALIDATION D1 presence greps all PASS (listed above).
- VALIDATION D2 absence greps all PASS (listed above).

---
*Phase: 125-docs-update-for-mvp-protocol*
*Completed: 2026-04-22*
