---
phase: 85
slug: documentation-refresh
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-05
---

# Phase 85 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Manual review + grep verification (documentation phase) |
| **Config file** | N/A |
| **Quick run command** | `grep -c "BlobNotify\|BlobFetch\|keepalive" db/PROTOCOL.md` |
| **Full suite command** | Cross-reference each wire format table against source code |
| **Estimated runtime** | ~5 seconds (grep checks) |

---

## Sampling Rate

- **After every task commit:** Verify document contains required keywords and sections
- **After every plan wave:** Full cross-reference of all four documents against implementation
- **Before `/gsd:verify-work`:** All DOC-01 through DOC-04 success criteria verified
- **Max feedback latency:** 5 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 85-01-01 | 01 | 1 | DOC-01 | grep | `grep -c "BlobNotify" db/PROTOCOL.md` | N/A | ⬜ pending |
| 85-02-01 | 02 | 1 | DOC-02 | grep | `grep -c "push" README.md` | N/A | ⬜ pending |
| 85-03-01 | 03 | 2 | DOC-03 | grep | `grep -c "auto_reconnect" sdk/python/README.md` | N/A | ⬜ pending |
| 85-03-02 | 03 | 2 | DOC-04 | grep | `grep -c "reconnect" sdk/python/docs/getting-started.md` | N/A | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

Existing infrastructure covers all phase requirements.

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Wire format tables match source code | DOC-01 | Byte-level accuracy requires human review | Compare BlobNotify/BlobFetch/BlobFetchResponse tables against peer_manager.cpp |
| Mermaid diagrams render correctly | DOC-01, DOC-02 | Visual rendering check | View on GitHub or Mermaid live editor |
| Getting-started tutorial flows logically | DOC-04 | Narrative coherence | Read through as a new user |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 5s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
