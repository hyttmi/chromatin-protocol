---
phase: 125
slug: docs-update-for-mvp-protocol
status: draft
nyquist_compliant: true
wave_0_complete: true
created: 2026-04-21
---

# Phase 125 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

This is a **documentation + code-hygiene** phase. Most tasks are doc writes
(no behavior change); the comment-hygiene pass touches source but must preserve
behavior. Validation is structured around doc-acceptance greps + a full test
suite gate after the comment-pass.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 v3.7.1 |
| **Config file** | `cli/build/CMakeCache.txt` + `build/db/CMakeCache.txt` (BUILD_TESTING=ON both) |
| **Quick run command** | `./build/cli/tests/cli_tests --reporter compact` |
| **Full suite command** | `./build/cli/tests/cli_tests && ./build/db/db/chromatindb_tests` |
| **Estimated runtime** | ~30-90s CLI + ~4m node (peer integration) |

---

## Sampling Rate

- **After every doc task:** Grep-based acceptance (e.g., `grep -c "signer_hint" db/PROTOCOL.md`)
- **After every comment-hygiene commit:** Run `./build/cli/tests/cli_tests --reporter compact` — quick regression check
- **Before final SUMMARY.md:** Full suite including `[peer]` — catches accidental code deletion from the D-14 hygiene pass

---

## Validation Dimensions

### D1. Content Presence (grep-based)

For each of the 4 target docs, acceptance is a set of grep patterns that MUST return ≥1 match:

**db/PROTOCOL.md** — MUST contain:
- `signer_hint` (≥ 3 matches)
- `PUBK-first` OR `PUBK_FIRST_VIOLATION`
- `BlobWrite` + `BlobWriteBody`
- `MsgType::Delete = 17` (retained)
- `MsgType::BlobWrite = 64`
- `BOMB` + `NAME` + `CDAT` + `CPAR` magics
- Error-code table header rows (0x07, 0x08, 0x09, 0x0A, 0x0B)

**README.md** — MUST contain:
- `ml-dsa-87` OR `ML-DSA-87`
- `libmdbx`
- `chunked`
- `peer`

**cli/README.md** — MUST contain:
- Every subcommand: `keygen`, `publish`, `put`, `get`, `rm`, `ls`, `contact`, `delegate`, `revoke`, `reshare`, `info`
- `--name`, `--type`, `--replace` flags
- A hello-world section heading

**db/ARCHITECTURE.md** (new) — MUST contain:
- All 8 DBIs listed by name
- `strand` OR `single writer thread`
- ASCII diagrams for handshake, ingest pipeline, BOMB cascade
- Cross-references to `db/PROTOCOL.md` sections

### D2. Content Absence (stale-reference greps)

All 4 docs + repo-wide (cli/src/, db/) — MUST have 0 matches for:
- `blob\.namespace_id\s*=` (removed field; the method `namespace_id()` is fine)
- `blob\.pubkey\.assign` OR `blob\.pubkey\s*=` (removed inline pubkey)
- `MsgType::Data` (deleted enum value)
- `TransportMsgType_Data` (after D-11 completes)
- `// Phase [0-9]+` in source code after D-13 pass (`cli/src/`, `db/`; test files gated by D-13 scope)
- `"(Phase [0-9]+)"` string literals in user-facing output (e.g., help text, error messages)

### D3. Behavioral preservation (comment-hygiene specific)

After each comment-hygiene commit (D-14 plan):
- `./build/cli/tests/cli_tests` exit 0, assertion count ≥ 197614 (current baseline)
- `./build/db/db/chromatindb_tests "[peer]"` exit 0, assertion count ≥ 506 (current baseline)

Any accidental code deletion during comment stripping shows up as a test failure
or assertion-count regression.

### D4. Cross-reference integrity

All doc-to-doc links use markdown section anchors. After final commit:
- No broken section anchors (automated via a simple grep + anchor-exists check)
- Every deep feature section has ≥1 cross-reference from the other 3 docs (enforces D-02 no-duplication)

### D5. Nyquist sampling cap

No per-task cap needed beyond D3's test-gate rule — doc writes are
non-behavioral, and the comment-hygiene pass is gated by full-suite run before
commit.

---

## Wave 0 Setup

Not applicable — builds already configured, test binaries exist, Catch2 integrated.

---

## Risks

- **D-14 over-aggression:** A "cleaned really well" pass could delete load-bearing
  WHY comments. Mitigation: D3 test gate catches silent code deletion; planner
  should preserve any comment citing an invariant, ADR, or bug ticket ID.
- **Doc drift during rewrite:** If code changes between research and
  docs-written, content may be stale. Mitigation: re-grep codebase at doc-commit
  time; treat any discrepancy as "code wins".
- **Grep-based acceptance is structural, not semantic:** A doc can pass every
  grep and still be bad prose. Mitigation: verifier (gsd-verifier) reads the
  docs end-to-end during phase verification and flags semantic gaps.
