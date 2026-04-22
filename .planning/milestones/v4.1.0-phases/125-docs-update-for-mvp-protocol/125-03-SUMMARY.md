---
phase: 125-docs-update-for-mvp-protocol
plan: 03
subsystem: docs
tags: [docs, architecture, storage, engine, net, dbis, strand, ingest, bomb, pubk-first, config, peer-management]

# Dependency graph
requires:
  - phase: 125-docs-update-for-mvp-protocol
    plan: 01
    provides: "db/PROTOCOL.md anchors for cross-linking (#blob-schema, #sending-a-blob-blobwrite--64, #pubk-first-invariant, #signer_hint-semantics, #owner_pubkeys-dbi, #name-blob-format, #bomb-blob-format, #errorresponse-type-63, #sync-protocol, #phase-c-blob-transfer, #chunked-transport-framing, #transport-layer, #role-signalling, #prometheus-metrics-endpoint, #hkdf-label-registry, #ttl-enforcement, #ingest-validation, #query-path-filtering, #expiry-arithmetic, #client-side-envelope-encryption, #syncnamespaceannounce-type-62, #quota-signaling)"
  - phase: 121-storage-concurrency-invariant
    provides: "TSAN ship-gate evidence cited in Storage Strand Model subsection"
  - phase: 122-schema-signing-cleanup-strip-namespace-and-compress-pubkey
    provides: "owner_pubkeys DBI + signer_hint resolution semantics"
  - phase: 123-tombstone-batching-and-name-tagged-overwrite
    provides: "BOMB magic, cascade side-effect, ttl=0 invariant, delegate-rejection rule"
  - phase: 118-configurable-constants-peer-management
    provides: "5 tunable knobs + 3 subcommands documented in README + ARCHITECTURE"
provides:
  - "db/ARCHITECTURE.md — the first internal implementation reference for chromatindb. Audience: contributors + implementers debugging unfamiliar code paths."
  - "db/README.md delta: 5 new Phase-118 config knobs with defaults, 3 peer-management subcommands, ARCHITECTURE.md cross-link, restart-verify deployment note, dropped '62 message types' stale count."
affects: [125-04, 125-05]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Implementation-doc cross-references to PROTOCOL.md by markdown section anchor (D-02/D-07 zero-duplication enforcement)"
    - "Source-of-truth file:line citations at the bottom of each section (drift-detection hook — code changes surface as a visible doc mismatch via grep)"
    - "Anchor verification: every PROTOCOL.md anchor used in ARCHITECTURE.md resolves to a real heading (verified by grep over db/PROTOCOL.md headings)"

key-files:
  created:
    - db/ARCHITECTURE.md
    - .planning/phases/125-docs-update-for-mvp-protocol/125-03-SUMMARY.md
  modified:
    - db/README.md

key-decisions:
  - "ARCHITECTURE.md landed at 879 lines (within the ~600-900 target); ~300 lines Storage + ~275 lines Engine + ~300 lines Net + Config (incl. top-matter + overview)"
  - "Expiry DBI documented with the actual key/value layout from storage.h:104 (key = [expiry_ts_be:8][content_hash:32], value = namespace:32) — not the interfaces-block layout (value was mis-described there)"
  - "signer_hint resolution documents the pubk_mismatch error path as requiring the recovered signing pubkey to hash to target_namespace (pins the PUBK invariant at the implementation level, not just at wire level)"
  - "Strand-model section cites Phase 121's VERIFICATION.md and TSAN-RESULTS.md as archaeological artifacts (not user-visible) to explain why the invariant matters; phase number appears only in the filesystem path, not in prose"
  - "db/README.md architecture paragraph explicitly mentions post-122 signing model + PUBK-first with forward-link to ARCHITECTURE.md; keeps the operator doc self-contained for ops purposes while pointing implementers at the deep section"
  - "Stale '62 message types' replaced with forward-looking language citing BlobWrite=64 and Delete=17 by wire number; avoids future doc drift if message types are added"

requirements-completed:
  - DOCS-03

# Metrics
duration: "8m"
completed: 2026-04-22
---

# Phase 125 Plan 03: db/ARCHITECTURE.md Creation + db/README.md Phase-118 Delta — Summary

**Created `db/ARCHITECTURE.md` (879 lines) as the first internal implementation reference for chromatindb — Storage (8 DBIs + strand model), Engine (11-step ingest pipeline + PUBK-first + BOMB cascade), Net (handshake, AEAD framing, PeerManager decomposition, sync orchestration) — and updated `db/README.md` with the 5 Phase-118 config knobs, 3 peer-management subcommands, a restart-verify deployment note, ARCHITECTURE.md cross-link, and dropped the stale '62 message types' count.**

## Performance

- **Duration:** 8m 06s wall-clock (2026-04-22T03:12:10Z → 03:20:16Z)
- **Tasks:** 3 (all autonomous, no checkpoints)
- **Files created:** 1 (`db/ARCHITECTURE.md`, 879 lines)
- **Files modified:** 1 (`db/README.md`, 477 → 503 lines, +26 net)

## Accomplishments

- **db/ARCHITECTURE.md is the first internal implementation reference** for chromatindb. Audience: contributors + implementers debugging unfamiliar code paths. Top-down per D-04: Storage → Engine → Net, all ASCII per D-06, with a Configuration-and-Subsystems coda for operator-adjacent topics that need an implementation-side pointer.
- **All 8 DBIs documented with verified key/value layouts** from `db/storage/storage.h:100-108`, each with a paragraph explaining purpose and access pattern. Includes Phase-122's `owner_pubkeys` DBI with its role in the post-122 signing model.
- **Storage Strand Model subsection** (D-05): dedicated, ~40 lines, with an ASCII threading diagram, the `STORAGE_THREAD_CHECK()` macro semantics (NDEBUG-cheap), the offload-then-post-back canonical pattern reproduced verbatim, and a citation of the Phase 121 TSAN ship-gate evidence (`.planning/phases/121-storage-concurrency-invariant/121-VERIFICATION.md` + `121-TSAN-RESULTS.md`).
- **11-step ingest pipeline** (plus 0b, 1.5, 1.7, 2a, 2.5, 3.5 substeps) tabulated against actual `db/engine/engine.cpp` line numbers — reject codes traceable to engine.cpp:109 through engine.cpp:438. ASCII flow diagram showing the crypto-offload boundary and the post-back-to-ioc rule.
- **PUBK-First, signer_hint, BOMB cascade** each get a dedicated subsection. BOMB cascade includes the load-bearing wire-path rule (`BlobWrite=64` not `Delete=17`, with rationale tied to the Delete dispatcher's 36-byte-payload precondition) so external client implementers don't reimplement the Phase 124 Plan 05 Rule-1 bug.
- **Net layer covers handshake state machine (PQ + lightweight + PQRequired fallback), AEAD framing with the send-queue nonce-desync invariant** (critical for future contributors — the only way to break wire safety is to bypass `send_queue_`), PeerManager 6-component decomposition, three-phase sync orchestration, PEX + reconnect + inactivity, and role-aware ACL routing.
- **Configuration section (~200 lines)** documents the 5 Phase-118 tuning knobs with real defaults verified against `db/config/config.h:51-58` (`blob_transfer_timeout=600`, `sync_timeout=30`, `pex_interval=300`, `strike_threshold=10`, `strike_cooldown=300`) and SIGHUP-reloadability classification. Briefly names peer-management subcommands, identity management, metrics/observability, and ACL model — each cross-links to the operator-facing reference in db/README.md per D-02.
- **db/README.md updated** with the Phase-118 deltas: 5 new config knobs in both the JSON example and the field reference (with defaults + reload semantics), a new Peer Management subsection under Usage, an Architecture paragraph sentence documenting the post-122 signing model + PUBK-first with a forward-link to ARCHITECTURE.md, a stale "62 message types" count replaced with forward-looking wire-type-by-number language (`BlobWrite = 64`, `Delete = 17`), and a restart-verify deployment note (compare `info` Uptime vs binary mtime) distilled from the post-124 retrospective.

## Task Commits

Each task was committed atomically against `master`:

1. **Task 1: Storage section** — `c52dc3c1` (docs) — New file with top-matter, 3-tier overview, Storage (8 DBIs + transaction model + strand model + encryption-at-rest + expiry/quotas). 307 lines.
2. **Task 2: Engine Layer append** — `9c96dc94` (docs) — Ingest flow + per-step table, PUBK-First Enforcement, signer_hint Resolution (owner + delegate paths), BOMB Cascade (with BlobWrite=64 wire-path note), Thread-Pool Crypto Offload, Ingest Error Codes. 580 lines total (+274).
3. **Task 3: Net + Config + README delta** — `322cf561` (docs) — Net Layer (handshake, AEAD framing, PeerManager decomposition, sync orchestration, PEX/reconnect/inactivity, role-aware ACL) + Configuration/Subsystems section + db/README.md delta (5 knobs, 3 subcommands, cross-link, restart-verify note, "62 message types" drop). ARCHITECTURE.md 879 lines total (+299); db/README.md 477 → 503 lines (+26).

**Plan metadata commit:** (pending — final commit after this SUMMARY write + STATE/ROADMAP updates.)

## Files Created/Modified

- `db/ARCHITECTURE.md` — 879 lines, 3 top-level sections (Storage / Engine / Net) plus an Overview and a Configuration+Subsystems coda. 25 cross-links into db/PROTOCOL.md by section anchor.
- `db/README.md` — 477 → 503 lines. Diff: +1 Architecture sentence (post-122 signing model), +5 config fields in JSON example, +5 config-field bullets, +1 Peer Management subsection, +1 restart-verify paragraph under Deployment, 1 reworded Wire Protocol paragraph (dropped "62 message types", added ARCHITECTURE.md cross-link).
- `.planning/phases/125-docs-update-for-mvp-protocol/125-03-SUMMARY.md` — this file.

## ARCHITECTURE.md Section Inventory

| Section                                   | Lines |
|-------------------------------------------|-------|
| Overview (incl. 3-tier ASCII diagram)     | ~40   |
| Storage > Sub-databases (8 DBIs table)    | ~80   |
| Storage > Transaction Model               | ~35   |
| Storage > Strand Model (D-05)             | ~50   |
| Storage > Encryption at Rest              | ~30   |
| Storage > Expiry + Quotas                 | ~30   |
| Engine > Ingest Pipeline + reject table   | ~80   |
| Engine > PUBK-First Enforcement           | ~30   |
| Engine > signer_hint Resolution           | ~30   |
| Engine > BOMB Cascade                     | ~55   |
| Engine > Thread Pool Crypto Offload       | ~35   |
| Engine > Ingest Error Codes               | ~25   |
| Net > Handshake State Machine             | ~55   |
| Net > AEAD Framing + Chunked Sub-Frames   | ~35   |
| Net > PeerManager Decomposition           | ~30   |
| Net > Sync Orchestration                  | ~35   |
| Net > PEX + Reconnect + Inactivity        | ~30   |
| Net > Role-Aware Access Control           | ~20   |
| Config + Subsystems (knobs, subcommands,  |       |
|   identity, metrics, ACL)                 | ~90   |
| **Total**                                 | **879** |

## PROTOCOL.md Cross-Link Inventory (25 total)

| Anchor                                                | Consumed from section                       |
|-------------------------------------------------------|---------------------------------------------|
| `#client-side-envelope-encryption`                    | Storage > Encryption at Rest                |
| `#hkdf-label-registry`                                | Storage > Encryption at Rest; Net handshake |
| `#query-path-filtering`                               | Storage > Expiry and Quotas                 |
| `#expiry-arithmetic`                                  | Storage > Expiry and Quotas                 |
| `#quota-signaling`                                    | Storage > Expiry and Quotas                 |
| `#errorresponse-type-63`                              | Engine > Ingest Pipeline; Ingest Error Codes |
| `#ingest-validation`                                  | Engine > Ingest Pipeline                    |
| `#pubk-first-invariant`                               | Engine > PUBK-First Enforcement             |
| `#signer_hint-semantics`                              | Engine > signer_hint Resolution             |
| `#bomb-blob-format`                                   | Engine > BOMB Cascade                       |
| `#sending-a-blob-blobwrite--64`                       | Engine > BOMB Cascade (wire-path rule)      |
| `#namespace-delegation`                               | Storage > delegation DBI                    |
| `#transport-layer`                                    | Net Layer opening paragraph                 |
| `#role-signalling`                                    | Net > Handshake State Machine               |
| `#syncnamespaceannounce-type-62`                      | Net > Handshake (post-handshake announce)   |
| `#chunked-transport-framing`                          | Net > AEAD Framing                          |
| `#sync-protocol`                                      | Net > Sync Orchestration                    |
| `#owner_pubkeys-dbi`                                  | Storage > owner_pubkeys DBI                 |
| `#pubk-blob-format`                                   | Storage > owner_pubkeys DBI                 |
| `#prometheus-metrics-endpoint`                        | Config > Metrics and Observability          |

(Reverse-links: `README.md#configuration` and `README.md#encryption-at-rest-feature-bullet` cite the operator-facing doc from inside ARCHITECTURE.md where operator context is needed.)

## Validation Grep Results (VALIDATION.md D1/D2/D4)

**D1 presence greps (ALL PASS):**

- All 8 DBIs named in db/ARCHITECTURE.md: `blobs`, `sequence`, `expiry`, `delegation`, `tombstone`, `cursor`, `quota`, `owner_pubkeys` → ALL PASS
- `grep -qi "strand\|single writer thread\|STORAGE_THREAD_CHECK" db/ARCHITECTURE.md` → PASS
- `grep -qiE "## *Engine Layer|## Engine" db/ARCHITECTURE.md` → PASS
- `grep -qi "ingest" db/ARCHITECTURE.md` → PASS
- `grep -qi "PUBK-first\|PUBK_FIRST" db/ARCHITECTURE.md` → PASS
- `grep -qi "signer_hint" db/ARCHITECTURE.md` → PASS
- `grep -qi "BOMB cascade\|BOMB Cascade" db/ARCHITECTURE.md` → PASS
- `grep -q "thread pool\|thread_pool" db/ARCHITECTURE.md` → PASS
- `grep -qiE "## *Net Layer|## Net" db/ARCHITECTURE.md` → PASS
- `grep -qi "handshake" db/ARCHITECTURE.md` → PASS
- `grep -qi "PeerManager" db/ARCHITECTURE.md` → PASS
- `grep -c "PROTOCOL\.md#" db/ARCHITECTURE.md` → 25 (≥5 required)
- `grep -q "blob_transfer_timeout" db/README.md` → PASS
- `grep -q "strike_threshold" db/README.md` → PASS
- `grep -q "add-peer" db/README.md` → PASS
- `grep -q "remove-peer" db/README.md` → PASS
- `grep -q "list-peers" db/README.md` → PASS
- `grep -q "ARCHITECTURE\.md" db/README.md` → PASS

**D2 absence greps (ALL PASS):**

- `! grep -qF "62 message types" db/README.md` → PASS (replaced with forward-looking language)

**D4 anchor-integrity spot check (ALL PASS):**

All 20 unique PROTOCOL.md anchors used in ARCHITECTURE.md resolve to real headings in db/PROTOCOL.md. Verified by:

- simple heading-match grep: 15 of 20 anchors match directly
- remaining 5 anchors (`#client-side-envelope-encryption`, `#errorresponse-type-63`, `#pubk-first-invariant`, `#sending-a-blob-blobwrite--64`, `#syncnamespaceannounce-type-62`) verified by direct inspection of db/PROTOCOL.md headings at lines 1421, 625, 386, 281, 602 respectively — all correspond to GFM-slugified versions of the headings present there. (The simple grep missed them because GFM slugification drops `(...)` groups and normalizes `=` → `--`; e.g. `### Sending a Blob (BlobWrite = 64)` → `#sending-a-blob-blobwrite--64`.)
- consistency cross-check: Plan 02 SUMMARY explicitly records `#errorresponse-type-63` as the verified slug from Plan 01's landed heading.

No broken anchors.

## Decisions Made

Decisions are enumerated in frontmatter `key-decisions`. The three load-bearing ones:

- **Expiry DBI key layout corrected mid-task.** The plan's `<interfaces>` block described expiry as `(expiry_ts:8 BE || content_hash:32) → 0-byte value`; the actual `db/storage/storage.h:104` shape is `(expiry_ts_be:8 || content_hash:32) → namespace:32`. Documented the correct value layout (namespace is useful for the scanner because it can delete from `blobs` without re-decoding the blob). Treated as a code-wins reconciliation per CONTEXT.md D-15.
- **Strand-model evidence cites Phase 121 as a filesystem archaeological artifact, not user-visible prose.** Phase numbers stay out of user-visible output per `feedback_no_phase_leaks_in_user_strings.md`, but ARCHITECTURE.md is a contributor-facing internal doc, and the citation takes the form of a `.planning/` path; the path contains the phase number but the prose does not. Consistent with the plan's explicit guidance and with how Plan 01 handles the Phase 121 TSAN evidence.
- **"62 message types" replaced with forward-looking language.** The count was already stale (post-phase-124 wire has additional types); rather than fix the count to an integer that will rot again, the Wire Protocol paragraph now names representative types by wire number (`BlobWrite = 64`, `Delete = 17`) and defers the full enumeration to PROTOCOL.md. Matches Plan 02's equivalent fix in README.md.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Expiry DBI value-layout correction**

- **Found during:** Task 1 Storage section drafting.
- **Issue:** Plan's `<interfaces>` block described the expiry DBI value as 0-byte (empty). Actual shape per `db/storage/storage.h:104` is `(expiry_ts_be:8 || content_hash:32) → namespace:32`.
- **Fix:** Documented the correct value layout in the 8-DBI table and in the per-DBI prose. Noted the scanner uses the value to delete from `blobs` without re-decoding.
- **Files modified:** `db/ARCHITECTURE.md`.
- **Commit:** folded into `c52dc3c1` (Task 1).

**2. [Rule 3 - Blocking] Ingest step re-numbering against actual engine.cpp**

- **Found during:** Task 2 Engine section drafting.
- **Issue:** Plan's `<interfaces>` step list used 0a/0b/0c/0d/0e/1/2/3/4/4.5/5/6 ordering; the actual `db/engine/engine.cpp` uses 0/0b/0c/0d/0e/1/1.5/1.7/2/2a/2.5/3/3.5/4/4.5. The differences: PUBK-first is Step 1.5 (before crypto offload), BOMB structural is Step 1.7, signer_hint resolve is Step 2 (after the gates, not Step 4), storage write is Step 4.
- **Fix:** Rebuilt the per-step reject table against the real source lines (engine.cpp:109 through engine.cpp:438). Preserved the plan's reject-code semantics (the table rows agree on wire error codes and reject triggers; only the step numbering changed).
- **Files modified:** `db/ARCHITECTURE.md`.
- **Commit:** folded into `9c96dc94` (Task 2).

**Total deviations:** 2 auto-fixed (Rule 1 + Rule 3), 0 architectural.
**Impact on plan:** None — both deviations tighten the doc to match code ground truth, which is the plan's explicit goal (CONTEXT.md D-15: "code wins").

## Issues Encountered

- **PreToolUse Read-before-edit reminders** on `db/ARCHITECTURE.md` (during Task 3's Net+Config append) and `db/README.md` (during each of the five successive edits). Both files had been read in the current session — ARCHITECTURE.md via a tail Read before the Net edit, README.md via the full-file Read at startup. Treated as non-blocking defensive hooks; each Edit succeeded on first attempt and the Write/Edit tools confirmed updates. No data loss.

## User Setup Required

None — documentation-only change.

## Next Phase Readiness

- **Plan 04 (D-11 flatc regen + `Data = 8` enum removal)** is orthogonal to this plan's output. ARCHITECTURE.md cites `TransportMsgType_BlobWrite = 64` and does not reference the deleted `Data = 8` slot anywhere, so Plan 04's enum removal will not affect ARCHITECTURE.md.
- **Plan 05 (D-13 + D-14 comment hygiene pass)** also orthogonal. The source-of-truth file:line citations at the bottom of each ARCHITECTURE section are drift-detectors for code changes; they do not cite specific comments and therefore survive comment removal unchanged.
- **Public doc surface now complete** for external readers: README.md (pitch) + cli/README.md (cdb user guide) + db/PROTOCOL.md (wire spec) + db/ARCHITECTURE.md (implementation ref) + db/README.md (operator guide). Per D-01/D-02 all five files are self-contained; every cross-file link is a section-anchor reference, not a content duplication.

## Self-Check: PASSED

- `db/ARCHITECTURE.md` exists on disk (879 lines).
- `db/README.md` exists on disk (503 lines).
- `.planning/phases/125-docs-update-for-mvp-protocol/125-03-SUMMARY.md` exists on disk (this file).
- All three task commits (`c52dc3c1`, `9c96dc94`, `322cf561`) verified present in `git log --oneline`.
- VALIDATION.md D1 presence greps: all PASS (listed above).
- VALIDATION.md D2 absence greps: all PASS.
- VALIDATION.md D4 anchor integrity: all 20 unique PROTOCOL.md anchors resolve.

---
*Phase: 125-docs-update-for-mvp-protocol*
*Completed: 2026-04-22*
