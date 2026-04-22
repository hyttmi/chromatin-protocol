---
phase: 127-nodeinforesponse-capability-extensions
plan: 02-requirements-text-update
subsystem: docs

tags: [requirements, nodeinforesponse, rate-limit, planning-doc]

requires:
  - phase: 127-nodeinforesponse-capability-extensions
    provides: D-03 governing decision (rename + retype rate-limit wire field)

provides:
  - REQUIREMENTS.md NODEINFO-03 line aligned with D-03 (rate_limit_bytes_per_sec u64 BE)
  - Wire/metrics/config symmetry primed for Phase 128's chromatindb_config_rate_limit_bytes_per_sec gauge

affects:
  - 127-01-dispatcher-encoder (same field name on node encoder side)
  - 127-03-cli-decoder (same field name on CLI decoder side)
  - 128-configurable-blob-cap (METRICS-01 reuses the name chromatindb_config_rate_limit_bytes_per_sec)

tech-stack:
  added: []
  patterns:
    - "REQUIREMENTS.md REQ lines cite the Phase CONTEXT.md decision ID when a planner-approved deviation retypes/renames a wire field"

key-files:
  created:
    - .planning/phases/127-nodeinforesponse-capability-extensions/127-02-SUMMARY.md
  modified:
    - .planning/REQUIREMENTS.md

key-decisions:
  - "REQUIREMENTS.md NODEINFO-03 now declares rate_limit_bytes_per_sec (u64 BE), matching the config field the node actually enforces — ghost-metric messages/sec removed from the v4.2.0 spec."

patterns-established:
  - "Planning-doc drift guard: the REQ line points to the governing CONTEXT.md decision (D-03) for auditability without duplicating the full rationale."

requirements-completed: [NODEINFO-03]

duration: ~4min
completed: 2026-04-22
---

# Phase 127 Plan 02: Requirements Text Update Summary

**REQUIREMENTS.md NODEINFO-03 retyped + renamed to `rate_limit_bytes_per_sec` (u64 BE) per CONTEXT.md D-03, aligning the v4.2.0 wire spec with the config field the node actually enforces.**

## Performance

- **Duration:** ~4 min
- **Started:** 2026-04-22T11:27:00Z (approx)
- **Completed:** 2026-04-22T11:30:55Z
- **Tasks:** 1 / 1
- **Files modified:** 1

## Accomplishments
- Single canonical REQ-ID line updated — no collateral touching of NODEINFO-01/02/04 or the traceability table.
- Automated verify gate passes cleanly (`grep -c 'rate_limit_messages_per_second' == 0`, `grep -c 'rate_limit_bytes_per_sec' >= 1`, canonical NODEINFO-03 prefix still present, u64 BE annotation present).
- PROTOCOL.md and ROADMAP.md untouched (D-13 scope rule upheld; ROADMAP narrative refresh remains out of plan scope).

## Task Commits

1. **Task 1: Rewrite REQUIREMENTS.md NODEINFO-03 line per D-03** — `2a94f3b3` (docs)

_No final metadata commit is required for worktree executors — orchestrator owns the merge._

## Files Created/Modified
- `.planning/REQUIREMENTS.md` — NODEINFO-03 bullet on line 26 rewritten to cite `rate_limit_bytes_per_sec` (u64 BE) with a D-03 audit pointer.

## Decisions Made
- Retained a D-03 audit pointer in the REQ text (per the plan action block's intent) without quoting the old field name literally. This preserves planner traceability while satisfying AC1's zero-residue requirement.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug / plan-internal inconsistency] Dropped literal old-field-name citation from the replacement text**
- **Found during:** Task 1 (post-edit verification)
- **Issue:** The plan's action block (lines 86-88) specifies replacement text that contains the literal phrase `` `rate_limit_messages_per_second` (u32 BE) `` inside the "renamed + retyped from ..." audit clause. The plan's AC1 (line 106) and automated `<verify>` block (line 101) independently assert `grep -c 'rate_limit_messages_per_second' .planning/REQUIREMENTS.md` == 0. Both cannot be true simultaneously — the action text literally requires what the acceptance criterion forbids.
- **Fix:** Kept the "renamed + retyped" audit framing and the "per Phase 127 CONTEXT.md D-03" pointer (preserves planner intent and traceability) but replaced the literal old-field-name citation with "the original (u32 BE) spec". CONTEXT.md D-03 (lines 49-56) retains the full rename audit trail by itself, so no information is lost.
- **Files modified:** `.planning/REQUIREMENTS.md`
- **Verification:** All 7 acceptance criteria (AC1-AC7) now pass; the `<verify>` automated block prints `PASS`.
- **Committed in:** `2a94f3b3`

---

**Total deviations:** 1 auto-fixed (1 Rule 1 planner inconsistency)
**Impact on plan:** Zero functional impact. D-03 intent (rename + retype) landed exactly as specified. The audit trail is preserved via the explicit D-03 pointer in the REQ line and CONTEXT.md itself. No scope creep.

## Issues Encountered
None beyond the plan-internal inconsistency documented above.

## User Setup Required
None — doc-only plan, no external service configuration.

## Next Phase Readiness
- Plan 127-01 (dispatcher encoder) and Plan 127-03 (CLI decoder) now share a canonical REQ line that matches the D-03 wire-field name they must both emit/decode.
- Phase 128's METRICS-01 can reuse the name `chromatindb_config_rate_limit_bytes_per_sec` without further REQ churn.
- No blockers for the remaining Phase 127 plans.

## Self-Check

Verification of post-SUMMARY claims:

- REQUIREMENTS.md modified on disk: FOUND at `.planning/REQUIREMENTS.md` (line 26 updated).
- Task commit `2a94f3b3` in git log: verified below.
- No unintended deletions in the task commit: verified (diff --diff-filter=D empty).
- PROTOCOL.md and ROADMAP.md unchanged: verified (`git diff --stat` empty for both).

---
*Phase: 127-nodeinforesponse-capability-extensions*
*Plan: 02-requirements-text-update*
*Completed: 2026-04-22*
