---
plan: 127-02-requirements-text-update
phase: 127
type: execute
wave: 1
depends_on: []
files_modified:
  - .planning/REQUIREMENTS.md
autonomous: true
requirements: [NODEINFO-03]
must_haves:
  truths:
    - "REQUIREMENTS.md NODEINFO-03 text reads rate_limit_bytes_per_sec (u64 BE), not rate_limit_messages_per_second (u32 BE)"
    - "All other REQ IDs (NODEINFO-01/02/04, VERI-02, etc.) and the traceability table are unchanged"
    - "ROADMAP.md is NOT touched by this plan (that is a separate doc; D-13-style scope rule applies — only REQUIREMENTS.md NODEINFO-03 line changes)"
  artifacts:
    - path: ".planning/REQUIREMENTS.md"
      provides: "REQUIREMENTS.md with NODEINFO-03 text reflecting the D-03 rename + retype"
      contains: "rate_limit_bytes_per_sec"
  key_links:
    - from: ".planning/REQUIREMENTS.md NODEINFO-03 line"
      to: "db/peer/message_dispatcher.cpp encoder (plan 127-01) + cli/src/commands.cpp decoder (plan 127-03)"
      via: "Wire field name/type cited in all three places must match"
      pattern: "rate_limit_bytes_per_sec"
---

<objective>
Update `.planning/REQUIREMENTS.md` NODEINFO-03 to reflect CONTEXT.md D-03: the wire field is `rate_limit_bytes_per_sec` (u64 BE), not `rate_limit_messages_per_second` (u32 BE). This is the only REQ text change in Phase 127 and is parallelizable with Plan 127-01 (no code dependency).

Purpose: Keep REQUIREMENTS.md in sync with the implemented wire format. Phase 128's `chromatindb_config_rate_limit_bytes_per_sec` Prometheus gauge (METRICS-01) will reuse the same name — wire/metrics/config symmetry.

Output: Single line edit (plus any minimal wording adjustment if needed) in REQUIREMENTS.md.
</objective>

<execution_context>
@$HOME/.claude/get-shit-done/workflows/execute-plan.md
@$HOME/.claude/get-shit-done/templates/summary.md
</execution_context>

<context>
@.planning/phases/127-nodeinforesponse-capability-extensions/127-CONTEXT.md

<interfaces>
Current REQUIREMENTS.md NODEINFO-03 line (at `.planning/REQUIREMENTS.md` line 26, from the pre-plan file state):

```
- [ ] **NODEINFO-03**: `NodeInfoResponse` wire format adds `rate_limit_messages_per_second` (u32 BE)
```

CONTEXT.md D-03 rationale (verbatim, lines 49-56):
- `config.rate_limit_bytes_per_sec` is the value the node actually enforces (via `MessageDispatcher::set_rate_limits` and `SyncOrchestrator::set_rate_limit`).
- No code path tracks messages/sec anywhere. Exposing a `messages_per_second` wire field would either be a stub (always 0) or a semantic lie.
- u32 bytes/sec tops out at ~4 GiB/s which is borderline today; u64 future-proofs without cost.
- Phase 128's `chromatindb_config_*` Prometheus gauge will reuse the same name — wire/metrics/config symmetry.

CONTEXT.md line 56 action commitment:
> "REQUIREMENTS.md NODEINFO-03 line will be updated in Phase 127's docs-update step to reflect the new name and type. This is the only REQ text change in Phase 127."

Scope constraints:
- D-13 forbids PROTOCOL.md edits in Phase 127. REQUIREMENTS.md is a PLANNING document, not a protocol spec document — it IS in scope. D-13 does not apply here.
- ROADMAP.md Phase 127 block (line 735) still references the old name in a narrative bullet. ROADMAP.md narrative is NOT touched by this plan; it will be updated organically when Phase 127 is marked complete in the ROADMAP progress section. Only the canonical REQ-ID line in REQUIREMENTS.md is updated here.
- The traceability table (REQUIREMENTS.md lines 111-115 and surrounding) lists NODEINFO-03 by REQ ID — the ID is unchanged, so table rows are unchanged.
</interfaces>
</context>

<tasks>

<task type="auto" tdd="false">
  <name>Task 1: Rewrite REQUIREMENTS.md NODEINFO-03 line per D-03</name>
  <files>.planning/REQUIREMENTS.md</files>
  <read_first>
    - .planning/REQUIREMENTS.md (confirm the exact current NODEINFO-03 line text at line 26, and scan the file for any other occurrence of `rate_limit_messages_per_second` — there should only be one, on the NODEINFO-03 line)
    - .planning/phases/127-nodeinforesponse-capability-extensions/127-CONTEXT.md (D-03 lines 49-56 — rationale for rename and retype)
  </read_first>
  <action>
    Make exactly one textual substitution in `.planning/REQUIREMENTS.md`.

    Replace the line (currently line 26):

    ```
    - [ ] **NODEINFO-03**: `NodeInfoResponse` wire format adds `rate_limit_messages_per_second` (u32 BE)
    ```

    with:

    ```
    - [ ] **NODEINFO-03**: `NodeInfoResponse` wire format adds `rate_limit_bytes_per_sec` (u64 BE) — renamed + retyped from `rate_limit_messages_per_second` (u32 BE) per Phase 127 CONTEXT.md D-03; exposes the value the node actually enforces (`config.rate_limit_bytes_per_sec`) instead of a ghost metric the node does not track
    ```

    Do NOT modify any other line in REQUIREMENTS.md. In particular:
    - Do NOT touch the traceability table (the REQ ID `NODEINFO-03` is unchanged; the row is valid).
    - Do NOT touch NODEINFO-01, -02, -04, or any other section header.
    - Do NOT renumber fields.
    - Do NOT touch ROADMAP.md (the narrative Phase 127 bullet is a separate document; its refresh is organic to the phase-complete roadmap update, not a planner concern here).
    - Do NOT touch db/PROTOCOL.md (D-13).

    The rewritten line is self-referential (cites D-03 by name) — this is acceptable inside the planning doc because the audience is the planner / executor / reviewer, not end users. `feedback_no_phase_leaks_in_user_strings.md` governs cdb-facing strings, not planning metadata.
  </action>
  <verify>
    <automated>
      test $(grep -c 'rate_limit_messages_per_second' .planning/REQUIREMENTS.md) -eq 0 && test $(grep -c 'rate_limit_bytes_per_sec' .planning/REQUIREMENTS.md) -ge 1
    </automated>
  </verify>
  <acceptance_criteria>
    1. Zero residue of the old field name:
       `grep -c 'rate_limit_messages_per_second' .planning/REQUIREMENTS.md` == 0

    2. New name present at least once (the NODEINFO-03 line) in REQUIREMENTS.md:
       `grep -c 'rate_limit_bytes_per_sec' .planning/REQUIREMENTS.md` >= 1

    3. NODEINFO-03 line still begins with the canonical checkbox + REQ-ID prefix:
       `grep -c '^- \[ \] \*\*NODEINFO-03\*\*:' .planning/REQUIREMENTS.md` == 1

    4. The `u64 BE` type annotation is present on the NODEINFO-03 line:
       `grep -E '^- \[ \] \*\*NODEINFO-03\*\*:.*u64 BE' .planning/REQUIREMENTS.md | wc -l` == 1

    5. Other REQ IDs untouched — spot-check that NODEINFO-01, -02, -04 lines still exist and unchanged:
       `grep -c 'NODEINFO-01' .planning/REQUIREMENTS.md` >= 1 (traceability table + canonical line, so >=2 realistically; >=1 proves presence)
       `grep -c 'NODEINFO-04' .planning/REQUIREMENTS.md` >= 1

    6. No protocol spec edits (PROTOCOL.md is out of scope per D-13):
       `git diff --stat db/PROTOCOL.md` prints no changes.

    7. No ROADMAP.md edits in this plan:
       `git diff --stat .planning/ROADMAP.md` prints no changes.
  </acceptance_criteria>
  <done>
    NODEINFO-03 text in `.planning/REQUIREMENTS.md` cites `rate_limit_bytes_per_sec (u64 BE)`. No other file is modified.
  </done>
</task>

</tasks>

<threat_model>
## Trust Boundaries

| Boundary | Description |
|----------|-------------|
| (none — doc-only plan) | This plan edits a planning-tier Markdown file. No code paths, no network surface, no data flow is altered. |

## STRIDE Threat Register

| Threat ID | Category | Component | Disposition | Mitigation Plan |
|-----------|----------|-----------|-------------|-----------------|
| T-127-04 | R (Repudiation / doc drift) | REQUIREMENTS.md NODEINFO-03 line | mitigate | Replacement line explicitly cites D-03 as the source of the rename, making the drift auditable. Planner traceability preserved. |
</threat_model>

<verification>
- Grep checks above prove the substitution landed cleanly.
- No build / test run needed — this is a single-line Markdown edit.
</verification>

<success_criteria>
1. `grep -c 'rate_limit_messages_per_second' .planning/REQUIREMENTS.md` == 0.
2. `grep 'NODEINFO-03.*rate_limit_bytes_per_sec.*u64 BE' .planning/REQUIREMENTS.md` returns exactly one line.
3. No other files are modified.
</success_criteria>

<output>
After completion, create `.planning/phases/127-nodeinforesponse-capability-extensions/127-02-SUMMARY.md`.
</output>
</content>
</invoke>