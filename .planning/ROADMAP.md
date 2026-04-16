# Roadmap: chromatindb v4.1.0 CLI Polish + Node Improvements

## Overview

Make chromatindb practical for enterprise secure file sharing across sites. Seven phases: CLI ergonomics and contact groups first (zero risk, unblocks @team testing), then node-side blob type indexing (unblocks ls filtering and chunked file types), node configuration and peer management (independent node-only work), chunked large files (depends on type indexing for CPAR/CDAT), request pipelining (primary customer is chunked downloads), documentation (after all features stable), and E2E verification against live node.

## Phases

**Phase Numbering:**
- Continues from v4.0.0 (Phases 111-115)
- v4.1.0 starts at Phase 116

- [x] **Phase 116: CLI Rename + Contact Groups** - Rename executable to `cdb`, implement contact group CRUD with SQLite schema versioning (completed 2026-04-16)
- [ ] **Phase 117: Blob Type Indexing + ls Filtering** - Node indexes blob types on ingest, ListRequest type filter, `cdb ls` hides infrastructure blobs
- [ ] **Phase 118: Configurable Constants + Peer Management** - Move 10 hardcoded constants to config.json with SIGHUP reload, add peer management CLI
- [ ] **Phase 119: Chunked Large Files** - Upload/download files >500 MiB via CDAT chunks + CPAR manifest with envelope v2 truncation prevention
- [ ] **Phase 120: Request Pipelining** - Multi-blob pipelined downloads over single PQ connection
- [ ] **Phase 121: Documentation** - PROTOCOL.md, README.md, cli/README.md updated with all v4.1.0 features
- [ ] **Phase 122: Verification** - Unit tests for all new features + E2E verification against live node at 192.168.1.73

## Phase Details

### Phase 116: CLI Rename + Contact Groups
**Goal**: Users can manage contacts and groups from a `cdb` command, enabling `--share @team` workflow for enterprise file distribution
**Depends on**: Nothing (first phase of v4.1.0)
**Requirements**: ERGO-01, CONT-01, CONT-02, CONT-03, CONT-04, CONT-05
**Success Criteria** (what must be TRUE):
  1. User runs `cdb` as the primary executable name (not a symlink to something else)
  2. User can create a named contact group, add/remove contacts, and see the group membership
  3. User can share a file with all members of a group using `cdb put --share @groupname file`
  4. User can bulk-import contacts from a JSON file with `cdb contact import team.json`
  5. SQLite database has a `schema_version` table that tracks the current schema version for future migrations
**Plans**: 2 plans
Plans:
- [x] 116-01-PLAN.md — Rename CLI to cdb + schema migration + test infrastructure
- [x] 116-02-PLAN.md — Group CRUD commands + share resolution + import/export

### Phase 117: Blob Type Indexing + ls Filtering
**Goal**: Users can filter blob listings by type, and `cdb ls` presents a clean view by hiding infrastructure blobs (CDAT chunks, PUBK, delegations)
**Depends on**: Phase 116
**Requirements**: TYPE-01, TYPE-02, TYPE-03, TYPE-04, ERGO-02, ERGO-03
**Success Criteria** (what must be TRUE):
  1. Node indexes the first 4 bytes of blob data as `blob_type` on every ingest without requiring any code change for new types
  2. `cdb ls` output hides infrastructure blobs (CDAT, PUBK, delegation types) by default, showing only user data
  3. `cdb ls --raw` shows all blobs including infrastructure types with their 4-byte type prefix visible
  4. ListRequest accepts an optional type filter and ListResponse includes the 4-byte type per entry
**Plans**: 2 plans
Plans:
- [x] 116-01-PLAN.md — Rename CLI to cdb + schema migration + test infrastructure
- [x] 116-02-PLAN.md — Group CRUD commands + share resolution + import/export

### Phase 118: Configurable Constants + Peer Management
**Goal**: Operators can tune node behavior via config.json and manage peers from the command line without editing config files manually
**Depends on**: Phase 116
**Requirements**: CONF-01, CONF-02, CONF-03, PEER-01, PEER-02, PEER-03
**Success Criteria** (what must be TRUE):
  1. All 10 previously hardcoded sync/peer constants are configurable in config.json with sensible defaults that match prior behavior
  2. Changed config values take effect after SIGHUP without node restart (where safe to reload)
  3. Invalid config values are rejected with clear error messages and range check details
  4. Operator can add, remove, and list peers using `chromatindb add-peer`, `remove-peer`, and `list-peers` subcommands
  5. `add-peer` and `remove-peer` modify config.json and trigger SIGHUP automatically
**Plans**: 2 plans
Plans:
- [x] 116-01-PLAN.md — Rename CLI to cdb + schema migration + test infrastructure
- [ ] 116-02-PLAN.md — Group CRUD commands + share resolution + import/export

### Phase 119: Chunked Large Files
**Goal**: Users can upload and download files larger than 500 MiB without full memory buffering, with automatic chunk management and truncation prevention
**Depends on**: Phase 117
**Requirements**: CHUNK-01, CHUNK-02, CHUNK-03, CHUNK-04, CHUNK-05
**Success Criteria** (what must be TRUE):
  1. User can upload a file >500 MiB and the CLI streams it in chunks without buffering the entire file in memory
  2. Upload produces CDAT chunk blobs plus a CPAR manifest blob that references all chunks
  3. Download of a CPAR manifest automatically reassembles all referenced chunks into the original file
  4. `cdb rm` of a manifest blob deletes the manifest and all associated CDAT chunk blobs
  5. Envelope format v2 includes segment count, preventing a truncation attack where an attacker drops trailing chunks
**Plans**: 2 plans
Plans:
- [ ] 116-01-PLAN.md — Rename CLI to cdb + schema migration + test infrastructure
- [ ] 116-02-PLAN.md — Group CRUD commands + share resolution + import/export

### Phase 120: Request Pipelining
**Goal**: Multi-blob downloads complete faster by pipelining requests over a single PQ connection instead of sequential round-trips
**Depends on**: Phase 119
**Requirements**: PIPE-01, PIPE-02, PIPE-03
**Success Criteria** (what must be TRUE):
  1. Downloading multiple blobs uses pipelined requests over a single PQ-encrypted connection (no new handshake per blob)
  2. Single reader thread invariant is maintained -- no concurrent recv on the same connection (AEAD nonce desync prevention)
  3. Pipeline depth is configurable with a sensible default of 8 in-flight requests
**Plans**: 2 plans
Plans:
- [ ] 116-01-PLAN.md — Rename CLI to cdb + schema migration + test infrastructure
- [ ] 116-02-PLAN.md — Group CRUD commands + share resolution + import/export

### Phase 121: Documentation
**Goal**: All v4.1.0 features are documented in PROTOCOL.md, README.md, and cli/README.md so operators and users have accurate reference material
**Depends on**: Phase 120
**Requirements**: DOCS-01, DOCS-02, DOCS-03, DOCS-04
**Success Criteria** (what must be TRUE):
  1. PROTOCOL.md documents the blob type indexing wire format (4-byte type field in ingest, ListRequest filter, ListResponse type per entry)
  2. PROTOCOL.md documents the new ListRequest/ListResponse format with type filtering
  3. README.md documents all new node config fields (10 constants) and peer management CLI subcommands
  4. cli/README.md documents contact groups, contact import, chunked file upload/download, request pipelining, and ls filtering
**Plans**: 2 plans
Plans:
- [ ] 116-01-PLAN.md — Rename CLI to cdb + schema migration + test infrastructure
- [ ] 116-02-PLAN.md — Group CRUD commands + share resolution + import/export

### Phase 122: Verification
**Goal**: All new functionality is proven correct through unit tests and a full end-to-end workflow against the live production node
**Depends on**: Phase 121
**Requirements**: VERI-01, VERI-02, VERI-03
**Success Criteria** (what must be TRUE):
  1. All new node features (blob type indexing, configurable constants, peer management) have Catch2 unit tests that pass
  2. All new CLI features (groups, import, chunking, pipelining, ls filtering) have unit tests that pass
  3. E2E verification against live node at 192.168.1.73 completes full workflow: put chunked file with group sharing, pipelined get, ls filtering, peer management
**Plans**: 2 plans
Plans:
- [ ] 116-01-PLAN.md — Rename CLI to cdb + schema migration + test infrastructure
- [ ] 116-02-PLAN.md — Group CRUD commands + share resolution + import/export

## Progress

**Execution Order:**
Phases execute in numeric order: 116 -> 117 -> 118 -> 119 -> 120 -> 121 -> 122

Note: Phase 118 depends only on Phase 116 (not 117), so it could execute in parallel with Phase 117 if desired.

| Phase | Plans Complete | Status | Completed |
|-------|----------------|--------|-----------|
| 116. CLI Rename + Contact Groups | 2/2 | Complete    | 2026-04-16 |
| 117. Blob Type Indexing + ls Filtering | 0/0 | Not started | - |
| 118. Configurable Constants + Peer Management | 0/0 | Not started | - |
| 119. Chunked Large Files | 0/0 | Not started | - |
| 120. Request Pipelining | 0/0 | Not started | - |
| 121. Documentation | 0/0 | Not started | - |
| 122. Verification | 0/0 | Not started | - |
