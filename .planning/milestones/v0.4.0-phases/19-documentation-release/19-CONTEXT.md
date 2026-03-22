# Phase 19: Documentation & Release - Context

**Gathered:** 2026-03-12
**Status:** Ready for planning

<domain>
## Phase Boundary

Operator can deploy and interact with chromatindb using documented procedures. Deliverables: db/README.md with config schema, startup, wire protocol overview, and deployment scenarios; interaction samples file demonstrating programmatic usage; version.h bumped to 0.4.0. Root README becomes a minimal pointer.

</domain>

<decisions>
## Implementation Decisions

### README relocation
- Root README becomes a minimal pointer: 2-3 paragraph intro + "See db/README.md for full documentation"
- ALL current root README content moves to db/README.md (crypto stack, architecture, build/test, usage, config, deployment scenarios, benchmarks — everything)
- db/README.md is the comprehensive operator reference

### Interaction samples
- Protocol walkthrough document, NOT runnable code or a C++ integration example
- Covers at minimum: connect, PQ handshake, store a signed blob, retrieve it
- Lives as a separate file (not inline in README), linked from db/README.md

### Claude's Discretion
- Walkthrough presentation format (sequence diagrams vs step-by-step narrative vs hybrid)
- Which additional interactions to cover beyond core flow (sync, pub/sub, delegation)
- Interaction samples filename (db/PROTOCOL.md, db/INTERACTIONS.md, etc.)
- Wire protocol overview depth (high-level vs message-level detail)
- Whether to reference FlatBuffers schemas or keep protocol docs conceptual
- Whether to include transport framing (AEAD frame format) or just application messages
- Where wire protocol overview lives (README section vs in the interactions file)
- Which config fields to document (based on what's actually implemented by Phase 18)
- Which deployment scenarios to include (existing three + any new v0.4.0 scenarios)
- Versioning narrative — whether to keep milestone-era section naming or do a full feature refresh
- Whether to add a CHANGELOG.md or rely on README + git history
- License section placement (db/ README, root only, or both)

</decisions>

<specifics>
## Specific Ideas

- Root README currently has 218 lines of comprehensive docs — this is the content baseline for db/README.md
- Config struct in db/config/config.h has undocumented fields: max_storage_bytes, rate_limit_bytes_per_sec, rate_limit_burst, sync_namespaces — document based on what's shipped
- version.h is currently at 0.1.0, needs bump to 0.4.0
- README currently has "v3.0 Features" section — reconcile with 0.4.0 version narrative

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- Root README.md (218 lines): comprehensive docs already written — move and augment, don't rewrite from scratch
- db/config/config.h: Config struct with all fields, doc comments, defaults — source of truth for config reference
- db/wire/codec.h: BlobData struct, encode/decode/signing functions — source for protocol documentation
- db/wire/transport_generated.h, blob_generated.h: FlatBuffers generated headers — wire format reference

### Established Patterns
- Config: JSON file + CLI args override pattern (--config, --data-dir, --log-level)
- Wire: FlatBuffers with deterministic encoding, canonical signing input (SHA3-256 of concatenation)
- Transport: ML-KEM-1024 handshake → ChaCha20-Poly1305 AEAD session, length-prefixed frames

### Integration Points
- db/version.h: single file for version bump (VERSION_MAJOR/MINOR/PATCH + VERSION string)
- db/main.cpp: CLI subcommands (keygen, run, version) — document these as the user interface
- Tests: version bump requires all tests passing (DOC-04 success criterion)

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 19-documentation-release*
*Context gathered: 2026-03-12*
