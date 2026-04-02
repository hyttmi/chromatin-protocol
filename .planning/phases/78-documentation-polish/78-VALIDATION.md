---
phase: 78
slug: documentation-polish
status: draft
nyquist_compliant: true
wave_0_complete: true
created: 2026-04-02
---

# Phase 78 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Manual content verification (docs phase — no code tests) |
| **Config file** | none |
| **Quick run command** | `grep -c "envelope\|encrypt\|HKDF" db/PROTOCOL.md sdk/python/README.md sdk/python/docs/getting-started.md` |
| **Full suite command** | `cd sdk/python && pytest tests/ -x -q` (regression only) |
| **Estimated runtime** | ~8 seconds |

---

## Sampling Rate

- **After every task commit:** Run `grep` content checks on modified doc files
- **After every plan wave:** Run full SDK test suite (regression gate)
- **Before `/gsd:verify-work`:** Full suite must be green + doc content checks pass
- **Max feedback latency:** 10 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 78-01-01 | 01 | 1 | DOC-01 | content | `grep "envelope" db/PROTOCOL.md` | ✅ | ⬜ pending |
| 78-01-02 | 01 | 1 | DOC-02 | content | `grep "write_encrypted" sdk/python/README.md` | ✅ | ⬜ pending |
| 78-01-03 | 01 | 1 | DOC-03 | content | `grep "write_encrypted" sdk/python/docs/getting-started.md` | ✅ | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

Existing infrastructure covers all phase requirements. No test framework needed — this is a pure documentation phase.

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| PROTOCOL.md envelope format accuracy | DOC-01 | Byte offsets must match _envelope.py source | Compare format table against ENVELOPE_MAGIC, SUITE_*, struct.pack calls |
| Tutorial code examples run correctly | DOC-03 | Requires live relay connection | Run tutorial snippets against KVM test swarm |

---

## Validation Sign-Off

- [x] All tasks have `<automated>` verify or Wave 0 dependencies
- [x] Sampling continuity: no 3 consecutive tasks without automated verify
- [x] Wave 0 covers all MISSING references
- [x] No watch-mode flags
- [x] Feedback latency < 10s
- [x] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
