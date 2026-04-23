# Phase 130: CLI Auto-tuning — Context

**Gathered:** 2026-04-23 (inline, lean execution)
**Status:** Ready for planning

<domain>
## Phase Boundary

`cdb` auto-tunes its chunking behavior from the server's advertised `max_blob_data_bytes` — no hardcoded 16 MiB default, no hardcoded 256 MiB max, no hardcoded 400 MiB threshold, no manual operator tuning needed.

**In scope:**
- On connect, `cdb` sends `NodeInfoRequest` and caches `max_blob_data_bytes` for the session.
- `CHUNK_SIZE_BYTES_DEFAULT`, `CHUNK_SIZE_BYTES_MAX`, `CHUNK_THRESHOLD_BYTES` derive from the cached cap at connect time.
- Hardcoded 16 / 256 / 400 MiB literals in `cli/src/wire.h` + `cli/src/chunked.h` deleted.
- Manifest validator rejects CPAR manifests whose `chunk_size_bytes` exceeds the server cap.
- `MAX_CHUNKS` policy finalized (see D-05).
- VERI-06 UAT: user-delegated 64 MiB file put+get round-trip with SHA3-256 verification.

**Out of scope:**
- Mid-session cap change (cap is session-constant per CONTEXT.md D-01 of Phase 129 pattern; reconnect to pick up new cap).
- Server-side changes — Phase 127 already ships the advertised cap; Phase 128 already wires the server cap to live config.
- Documentation refresh → Phase 131 DOCS-02.

</domain>

<decisions>
## Implementation Decisions

### Session cap cache (CLI-01)
- **D-01:** Cache the advertised cap in the existing `Connection` object (or an explicit `Session` struct if one exists). Single field `uint64_t session_blob_cap_ = 0;` — seeded once per successful connect, treated as const for rest of session.
- **D-02:** The on-connect NodeInfoRequest/Response exchange is the FIRST protocol round-trip after handshake completes. Before this lands, any command that needs cap-aware chunking would fire without the cap — so the cache-seed is explicitly sequenced BEFORE the first command's protocol traffic.
- **D-03:** `cap == 0` (defensive/unknown) fallback: reject chunked operations with a clear error ("server cap not advertised — node older than v4.2.0? try a newer build"). Do NOT silently pick a default because silent default would re-introduce the same operator confusion Phase 130 is meant to eliminate.

### Chunking constant derivation (CLI-02, CLI-03, CLI-04)
- **D-04:** Replace the three hardcoded constants:
  - `CHUNK_SIZE_BYTES_DEFAULT` = `session_blob_cap_` (i.e., one blob per chunk; blobs are sized exactly at the cap). Simplifies: one chunk = one server-sized blob = one mdbx value.
  - `CHUNK_SIZE_BYTES_MAX` = `session_blob_cap_` (same thing; "max" and "default" collapse into one concept now).
  - `CHUNK_THRESHOLD_BYTES` = `session_blob_cap_` — files strictly larger than one blob get chunked; files ≤ cap go as a single blob.
- **D-05 (MAX_CHUNKS policy):** Retain `MAX_CHUNKS = 65536`. At 4 MiB default cap → 256 GiB max file; at 64 MiB max cap → 4 TiB max file. 4 TiB covers all practical use; 256 GiB covers 99.9% at default config. If operators need more, a future phase can bump to `1 << 20` with a compat ladder. YAGNI.

### Manifest validator (CLI-04)
- **D-06:** The existing CPAR manifest validator (`cli/src/wire.cpp:534`) compares `chunk_size_bytes` against `CHUNK_SIZE_BYTES_MAX`. Replace with comparison against `session_blob_cap_` (passed as a parameter or read from the Connection object the validator has access to). Manifests with `chunk_size_bytes > session_blob_cap_` fail validation with a clear diagnostic that names both values (seen vs allowed).

### Cap-less fallback
- **D-07:** If a pre-v4.2.0 node responds without the new fields (NodeInfoResponse payload ends before the offset Phase 127 established), the decoder MUST detect the short payload and refuse the session with: "node version too old — cdb v4.2.0+ requires server cap advertisement (phase 127+)". This is hard-fail, not silent fallback.

### Test strategy
- **D-08:** VERI-06 is user-delegated UAT (64 MiB round-trip against the live node). Writing a hermetic in-process 2-binary test is out-of-scope budget-wise and the user already runs `cdb --node local` + `--node home` tests manually.
- **D-09:** Unit tests in `cli/tests/` cover: (a) session-cap seed on connect (mock NodeInfoResponse), (b) chunking threshold derivation (cached cap → threshold / default / max), (c) manifest validator accept/reject at boundary, (d) cap-missing fallback rejects hard per D-07.

### Claude's Discretion
- Exact location of `session_blob_cap_` — `Connection` vs a new `Session` struct vs a static on an anonymous namespace. Executor picks the least-invasive option.
- Error message wording for D-03 / D-07 hard-fail paths.
- Whether the manifest validator takes cap as a param or reads it from a Connection reference.

</decisions>

<canonical_refs>
## Canonical References

### Project-level
- `.planning/REQUIREMENTS.md` — CLI-01..05, VERI-06
- `.planning/ROADMAP.md` §"Phase 130"
- `.planning/PROJECT.md` — YAGNI, no backward compat

### Phase-level carryover
- `.planning/phases/127-nodeinforesponse-capability-extensions/127-CONTEXT.md` — wire layout of the advertised cap field
- `.planning/phases/127-nodeinforesponse-capability-extensions/127-01-SUMMARY.md` — authoritative byte offset
- `.planning/phases/128-configurable-blob-cap-frame-shrink-config-gauges/128-CONTEXT.md` — the cap values that may appear on the wire (1 MiB .. 64 MiB range)

### Codebase anchors
- `cli/src/wire.h:301,303` — `CHUNK_SIZE_BYTES_DEFAULT` (delete), `CHUNK_SIZE_BYTES_MAX` (delete)
- `cli/src/chunked.h:50` — `CHUNK_THRESHOLD_BYTES` (delete)
- `cli/src/chunked.cpp:146,156,497` — callsites consuming the deleted constants (rewire to session cap)
- `cli/src/commands.cpp:703,715` — additional consumer callsites
- `cli/src/commands.cpp:2195-2306` — existing `cdb info` NodeInfoResponse decode (reference pattern for the cache-seed call path)
- `cli/src/wire.cpp:534` — manifest validator
- `cli/src/connection.h/cpp` — Connection object (candidate location for the session cap cache per D-01)
- `cli/tests/test_chunked.cpp` — existing chunked test file (extend for D-09 cases)
- `cli/tests/test_wire.cpp` — existing wire test file (extend for manifest validator tests)

</canonical_refs>

<deferred>
## Deferred Ideas

- **MAX_CHUNKS bump to 1 << 20** — reserve for a future phase if operators hit the 256 GiB / 4 TiB ceilings in practice.
- **Mid-session cap renegotiation** — explicitly out-of-scope (consistent with Phase 129's session-constant stance).
- **CLI-side cap enforcement at storage layer** — not needed; the node's Phase 128 ingest enforcement is authoritative. CLI just sizes its payloads correctly to avoid round-trip rejection.

</deferred>
