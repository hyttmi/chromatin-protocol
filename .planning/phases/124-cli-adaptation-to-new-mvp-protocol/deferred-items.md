# Deferred Items — Phase 124

## 2026-04-21 — plan 04

### Pre-existing phase-number leak in main.cpp help text

**Location:** `cli/src/main.cpp:619` — `"batched BOMB tombstone (Phase 123). Exit 2 if no targets given.\n"`

**Introduced:** commit 257b5f27 (plan 123-03 Task 1).

**Status:** Not fixed in plan 04 (out of scope — predates phase 124, not caused by
any task-1/2/3 changes). Memory constraint `feedback_no_phase_leaks_in_user_strings.md`
says user-visible cdb help strings must not contain phase numbers; this violates it.

**Recommendation:** Fix in Phase 125 (the documentation + cli/README.md sweep). The
fix is trivial — replace "(Phase 123)" with "" (or reword). Tracked here so it
doesn't get lost.

**Why not auto-fix in plan 04:** Plan 04's scope is auto-PUBK wiring, D-05 error
decoder, and D-06 BOMB cascade. This leak is unrelated to any of those and the
executor's scope-boundary rule says "only auto-fix issues directly caused by the
current task's changes." Logging here per the rule; do NOT fix in this plan.
