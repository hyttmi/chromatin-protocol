# Phase 74: Packaging & Documentation - Context

**Gathered:** 2026-03-31
**Status:** Ready for planning

<domain>
## Phase Boundary

SDK is release-ready with getting started tutorial, SDK README, pyproject.toml metadata polish, and PROTOCOL.md corrections. This is the final v1.6.0 phase — after this, the Python SDK milestone is complete.

Requirements: PKG-02 (getting started tutorial), DOCS-01 (README + PROTOCOL.md SDK section + HKDF fix)

</domain>

<decisions>
## Implementation Decisions

### Tutorial format & location
- **D-01:** Tutorial lives at `sdk/python/docs/getting-started.md`
- **D-02:** Covers core flow (identity setup, connect, write blob, read blob) plus 2-3 query examples (exists, metadata, namespace_list) — ~150 lines of example code
- **D-03:** Markdown walkthrough with copy-paste code blocks. Not Jupyter, not runnable scripts.

### README structure
- **D-04:** Create `sdk/python/README.md` with: install instructions, quick start (5-line connect+write+read), API overview table (all 15 client methods), link to tutorial
- **D-05:** Add one line to top-level `README.md` pointing to `sdk/python/README.md`
- **D-06:** `sdk/python/README.md` doubles as PyPI `long_description` (set `readme = "README.md"` in pyproject.toml)

### SDK packaging metadata
- **D-07:** Keep version at 0.1.0 (first public release, alpha)
- **D-08:** Add to pyproject.toml: `license = "MIT"`, author name/email, `urls.Homepage` and `urls.Repository`, classifiers (Development Status :: 3 - Alpha, post-quantum, database, asyncio)
- **D-09:** Add `readme = "README.md"` to `[project]` section for PyPI long_description

### PROTOCOL.md updates
- **D-10:** Fix HKDF salt discrepancy: change `SHA3-256(initiator_signing_pubkey || responder_signing_pubkey)` to empty salt (matching C++ implementation) in all occurrences
- **D-11:** Add short "SDK Client Notes" section documenting:
  - AEAD nonce starts at 1 after PQ handshake (nonce 0 consumed by auth exchange)
  - C++ relay sends Pong with request_id=0 (doesn't echo client's id)
  - Mixed endianness: BE for framing, LE for auth payload pubkey_size and canonical signing input (ttl/timestamp)
  - `chromatindb.exceptions.ConnectionError` inherits `ProtocolError` (not Python builtin `ConnectionError`)

### Claude's Discretion
- Tutorial prose style and section structure
- Exact API overview table columns and ordering
- Exact pyproject.toml classifier list
- SDK Client Notes section ordering and wording

</decisions>

<specifics>
## Specific Ideas

- Tutorial should assume the reader has a running chromatindb node accessible via relay — don't explain how to set up the node
- Quick start in README should be minimal enough to fit in a GitHub "about" glance — 5-line async context manager pattern
- The HKDF salt fix must be applied everywhere it appears in PROTOCOL.md (at least 2 occurrences: the handshake diagram and the prose description)

</specifics>

<canonical_refs>
## Canonical References

### Protocol documentation
- `db/PROTOCOL.md` — Wire protocol spec, HKDF section (lines 67-91 have the salt discrepancy), handshake diagrams

### SDK implementation (read for API reference accuracy)
- `sdk/python/chromatindb/client.py` — All 15 public client methods (write, read, delete, exists, ping + 10 query/pub-sub)
- `sdk/python/chromatindb/__init__.py` — All exported types and classes
- `sdk/python/chromatindb/identity.py` — Identity class for key generation/loading

### Packaging
- `sdk/python/pyproject.toml` — Current packaging config (needs metadata additions per D-08/D-09)

### Top-level README
- `README.md` — Currently 13 lines, needs SDK pointer per D-05

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `sdk/python/tests/test_integration.py` — Working integration test code showing real connect+write+read+query patterns — use as reference for tutorial examples
- `tools/test_vector_generator.cpp` — Shows canonical signing input format if tutorial needs crypto context

### Established Patterns
- All client methods follow async context manager pattern: `async with ChromatinClient(...) as client:`
- Identity setup: `identity = Identity.generate()` or `Identity.from_file(path)`
- Connect pattern: `ChromatinClient(host, port, identity)`

### Integration Points
- `sdk/python/README.md` is new (doesn't exist yet)
- `sdk/python/docs/` directory is new (doesn't exist yet)
- `README.md` needs one additional line
- `db/PROTOCOL.md` needs inline edits (salt fix + new section)
- `sdk/python/pyproject.toml` needs metadata additions (non-breaking)

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 74-packaging-documentation*
*Context gathered: 2026-03-31*
