# Roadmap: chromatindb v4.1.0 CLI Polish + Node Improvements

## Overview

Make chromatindb practical for enterprise secure file sharing across sites. Seven phases: CLI ergonomics and contact groups first (zero risk, unblocks @team testing), then node-side blob type indexing (unblocks ls filtering and chunked file types), node configuration and peer management (independent node-only work), chunked large files (depends on type indexing for CPAR/CDAT), request pipelining (primary customer is chunked downloads), documentation (after all features stable), and E2E verification against live node.

## Phases

**Phase Numbering:**
- Continues from v4.0.0 (Phases 111-115)
- v4.1.0 starts at Phase 116

- [x] **Phase 116: CLI Rename + Contact Groups** - Rename executable to `cdb`, implement contact group CRUD with SQLite schema versioning (completed 2026-04-16)
- [x] **Phase 117: Blob Type Indexing + ls Filtering** - Node indexes blob types on ingest, ListRequest type filter, `cdb ls` hides infrastructure blobs (completed 2026-04-16)
- [x] **Phase 118: Configurable Constants + Peer Management** - Move 5 hardcoded constants to config.json with SIGHUP reload, add peer management CLI (completed 2026-04-16)
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
- [x] 117-01-PLAN.md — Node-side type indexing: storage, wire protocol, dispatcher, tests
- [x] 117-02-PLAN.md — CLI ls filtering: type labels, hide list, --raw/--type flags

### Phase 118: Configurable Constants + Peer Management
**Goal**: Operators can tune node behavior via config.json and manage peers from the command line without editing config files manually
**Depends on**: Phase 116
**Requirements**: CONF-01, CONF-02, CONF-03, PEER-01, PEER-02, PEER-03
**Success Criteria** (what must be TRUE):
  1. All 5 operator-relevant hardcoded sync/peer constants are configurable in config.json with sensible defaults that match prior behavior
  2. Changed config values take effect after SIGHUP without node restart (where safe to reload)
  3. Invalid config values are rejected with clear error messages and range check details
  4. Operator can add, remove, and list peers using `chromatindb add-peer`, `remove-peer`, and `list-peers` subcommands
  5. `add-peer` and `remove-peer` modify config.json and trigger SIGHUP automatically
**Plans**: 2 plans
Plans:
- [x] 118-01-PLAN.md — Config fields, component integration, SIGHUP reload, pidfile, tests
- [x] 118-02-PLAN.md — Peer management subcommands (add-peer, remove-peer, list-peers)

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
- [ ] 119-01-PLAN.md — [To be planned]
- [ ] 119-02-PLAN.md — [To be planned]

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
- [ ] 120-01-PLAN.md — [To be planned]
- [ ] 120-02-PLAN.md — [To be planned]

### Phase 121: Documentation
**Goal**: All v4.1.0 features are documented in PROTOCOL.md, README.md, and cli/README.md so operators and users have accurate reference material
**Depends on**: Phase 120
**Requirements**: DOCS-01, DOCS-02, DOCS-03, DOCS-04
**Success Criteria** (what must be TRUE):
  1. PROTOCOL.md documents the blob type indexing wire format (4-byte type field in ingest, ListRequest filter, ListResponse type per entry)
  2. PROTOCOL.md documents the new ListRequest/ListResponse format with type filtering
  3. README.md documents all new node config fields (5 constants) and peer management CLI subcommands
  4. cli/README.md documents contact groups, contact import, chunked file upload/download, request pipelining, and ls filtering
**Plans**: 2 plans
Plans:
- [ ] 121-01-PLAN.md — [To be planned]
- [ ] 121-02-PLAN.md — [To be planned]

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
- [ ] 122-01-PLAN.md — [To be planned]
- [ ] 122-02-PLAN.md — [To be planned]

## Progress

**Execution Order:**
Phases execute in numeric order: 116 -> 117 -> 118 -> 119 -> 120 -> 121 -> 122

Note: Phase 118 depends only on Phase 116 (not 117), so it could execute in parallel with Phase 117 if desired.

| Phase | Plans Complete | Status | Completed |
|-------|----------------|--------|-----------|
| 116. CLI Rename + Contact Groups | 2/2 | Complete    | 2026-04-16 |
| 117. Blob Type Indexing + ls Filtering | 2/2 | Complete    | 2026-04-16 |
| 118. Configurable Constants + Peer Management | 2/2 | Complete    | 2026-04-16 |
| 119. Chunked Large Files | 0/0 | Not started | - |
| 120. Request Pipelining | 0/0 | Not started | - |
| 121. Documentation | 0/0 | Not started | - |
| 122. Verification | 0/0 | Not started | - |

## Backlog

### Phase 999.1: Role-signalled handshake + fix ACL classifier — ✅ RESOLVED 2026-04-18

**Status:** Completed inline on 2026-04-18 without a formal `/gsd-plan-phase` flow. All three observed bugs fixed; laptop↔server sync verified end-to-end in production; closed-mode ACL + live revocation tests pass (SIGSEGVs gone).

**Note on the design:** The final implementation carried the role byte in the AEAD-encrypted **AuthSignature payload** rather than the cleartext TrustedHello, which is stronger: the role is integrity-protected by the session keys, not visible to on-path observers, and the same machinery covers both lightweight and PQ handshakes without touching TrustedHello or KemPubkey wire layouts. Fail-closed on unknown role is preserved.

**Commits:**

| Commit | Change |
|---|---|
| `f5fa13bf` | Add `Role` enum with reserved slots (Peer/Client + Observer/Admin/Relay reserved) |
| `0eaa4377` | Role-signalled handshake: role byte in AuthSignature payload; both binaries updated; receiver routes ACL by declared role |
| `34a4ad31` | Refactor `PeerInfo::is_client` bool → `net::Role` enum; fixed both revocation SIGSEGVs as a side effect |
| `784b1260` | PROTOCOL.md: document the new AuthSignature layout + Role Signalling section |

**Production verification:**

- Laptop (337bf7...) connects to server (591782...); server log shows `handshake complete (responder, PQ, peer_role=peer)` and `Connected peer 1ec444e56d6afb24@192.168.1.173:59730`. Both sides classify correctly.
- Blob put on one node propagates to the other; tombstone from `cdb rm` also propagates.
- `cdb --node home` correctly negotiates `peer_role=client` against the server.

**Tests:**

- `[auth_helpers]` — 66 assertions, 12 cases, all pass (includes role round-trip + fail-closed on unknown role).
- `[handshake]` — 48 assertions, 9 cases, pass.
- `[peer]` — 75/76 pass. Two previously-failing SIGSEGVs (`closed mode rejects unauthorized peer`, `reload_config revokes connected peer`) fixed as intended side effect. Remaining failure is the pre-existing cosmetic `nodeinfo types_count` noted in project memory.

#### Original design context (preserved for posterity)

#### Observed bugs (all stem from the same classifier fragility)

1. **Sync broken in open mode (production-observed 2026-04-18).** Laptop and server complete PQ handshake against each other, but both ends log `Connected client` and `syncs=0` forever. Root cause: `db/peer/connection_manager.cpp:80-83` treats *any* TCP connection as a client when `allowed_client_keys` is empty, because `AccessControl::is_client_allowed()` (`db/acl/access_control.cpp:50-51`) returns `true` unconditionally in open mode. `is_client = true` makes the node skip sync, PEX, and dedup for what are actually peer connections.

2. **`test_peer_manager.cpp:213` — "closed mode rejects unauthorized peer"** (SIGSEGV). Closed-mode peer ACL should reject connections from identities not in `allowed_peer_keys`. Same classifier bug routes the connection through the wrong ACL path.

3. **`test_peer_manager.cpp:359` — "reload_config revokes connected peer"** (SIGSEGV). `ConnectionManager::disconnect_unauthorized_peers()` runs but returns without disconnecting because `is_client=true` routes the check through the open-mode client ACL (`connection_manager.cpp:371-373`).

**Repro:** `./build/db/chromatindb_tests "*reload*"` and `./build/db/chromatindb_tests "*closed*"`.

#### Design: role byte in TrustedHello

Extend TrustedHello payload to include an explicit 1-byte role prefix:

```
TrustedHello payload (new) = [role:1][nonce:32][pubkey:2592]
```

Role enum (reserve space for future use cases — cost of reserved slots is zero):

| Value | Name      | Purpose                                                    |
|-------|-----------|------------------------------------------------------------|
| 0x00  | PEER      | Full node-to-node replication (sync, PEX, dedup)           |
| 0x01  | CLIENT    | Read/write API access (blobs, queries, subscriptions)      |
| 0x02  | OBSERVER  | (reserved) Read-only — metrics, backup, auditors           |
| 0x03  | ADMIN     | (reserved) Privileged CLI — config reload, revoke          |
| 0x04  | RELAY     | (reserved) Bridge/relay node                               |
| 0x05..0xFE | —    | Reserved                                                    |
| 0xFF  | —         | Reserved (sentinel / error)                                 |

**Hard rule:** handshake MUST reject unknown role values (fail-closed), so old binaries can never misinterpret new ones.

#### Behaviour changes

- `chromatindb` node (initiator): peer-mode connect → send PEER. Reject inbound with unknown role.
- `cdb` CLI (initiator): always sends CLIENT.
- Receiver reads the role byte, stores `is_client` (and eventually `is_observer`, `is_admin`, ...) from the declared role.
- Each role has its own ACL allow-list lookup — no more "first match wins across lists."
- `disconnect_unauthorized_peers()` branches on role explicitly, fixing both revocation SIGSEGVs.

#### Scope of the change

- Wire format: new role byte in TrustedHello payload (`db/net/connection.cpp`, `db/PROTOCOL.md`).
- Size validation: update `expected_size` check in lightweight handshake (+1 byte).
- Role enum definition (new header, e.g. `db/net/role.h`).
- Initiator signalling: chromatindb node sends PEER, `cdb` CLI sends CLIENT.
- Receiver classification: remove ACL-based inference from `connection_manager.cpp:80-87`. Use received role.
- ACL reshape: distinct allow-lists per role (or at minimum keep current `allowed_peer_keys` / `allowed_client_keys` and look up based on role).
- Revocation path: fix `disconnect_unauthorized_peers` to use role rather than `is_client`.
- Tests: regression tests for the two SIGSEGVs, plus new tests for role signalling (peer-connects-as-peer, client-connects-as-client, unknown-role-rejected, closed-ACL-rejects-unlisted-role).
- PROTOCOL.md: document the new TrustedHello payload format.
- Both binaries ship together — no backward compatibility with pre-role handshake.

#### Immediate effect once shipped

- Laptop↔server sync works in open mode (both are PEER).
- Remote `cdb` over TCP works in closed-client-mode (explicit CLIENT role + client ACL check).
- Closed-mode peer rejection test passes (correct ACL routed).
- Live revocation test passes (correct ACL consulted during disconnect).

Plans:
- [x] Inline implementation (2026-04-18, commits f5fa13bf / 0eaa4377 / 34a4ad31 / 784b1260)

### Phase 999.2: cdb rm idempotency + target-existence check (BACKLOG)

**Goal:** Make `cdb rm` behave sanely when run multiple times against the same target, and give clear feedback when the target doesn't exist.

**Requirements:** TBD

**Plans:** 0 plans

Current behaviour (observed 2026-04-18):

- Each `cdb rm <hash>` signs a fresh tombstone blob. Because ML-DSA-87 signatures are non-deterministic, every invocation produces a different tombstone hash. Running `cdb rm` three times against the same target creates three distinct tombstones, all propagated via sync, all stored on every peer. Wasteful, and misleading to operators.
- cdb does not check whether the target blob exists before signing the tombstone. Typos, already-deleted targets, and accidental double-deletes all "succeed" with no feedback.
- `DeleteAck` has a status byte (0=stored, 1=duplicate) but cdb doesn't inspect it.

**Suggested direction:**

1. **Pre-check target existence** via an `Exists`/`GetBlob` probe before building the tombstone. If the target is not found, error out ("blob not found on node X") unless `--force` is passed.
2. **Surface the DeleteAck status** in cdb output: distinguish "target deleted" from "already tombstoned" clearly.
3. **Consider deterministic tombstones** — would require a hashing pre-image that doesn't include the signature, so two rm calls for the same target produce the same tombstone hash. Needs a protocol change and careful thought about security.
4. **Optional**: add `cdb rm --dry-run` to print what would be deleted without signing.

Also unresolved, related:

- **Plain `cdb ls` (no flags) hitting PQ handshake timeout** when `config.json` has `default_node: home` and the remote isn't reachable or stalls during PQ. Needs a separate investigation trace — server-side log during the stall, reachability check. Could be a lingering issue in the PQ responder on the node side, or could be a race with the cdb connect timeout. File under this phase or split out if the root cause is orthogonal.

Plans:
- [ ] TBD (promote with /gsd-plan-phase when ready)

### Phase 999.3: cdb put of the same file creates duplicate blobs (BACKLOG)

**Goal:** Decide and implement the right semantics for re-uploading the same file via `cdb put`. Today it creates a second blob with a different hash; the user expected overwrite/idempotency.

**Requirements:** TBD

**Plans:** 0 plans

**Observed (2026-04-18):**

```
./cdb put compile_commands.json --node home
4a872238127a227378aa70461cabea7c5d28896c4c6a281467ca2bb06840437e

./cdb put compile_commands.json --node home
f375f5c3bb7fcfe0c19d214d55cabf509d63c4f5bfda3e3bd5bf27410cd433f1

./cdb ls --node home
4a872238127a227378aa70461cabea7c5d28896c4c6a281467ca2bb06840437e  CENV
f375f5c3bb7fcfe0c19d214d55cabf509d63c4f5bfda3e3bd5bf27410cd433f1  CENV
```

Same file, different hashes, both stored side-by-side.

**Why it happens:**

The blob hash is `SHA3-256(namespace || data || ttl || timestamp)`. Even for byte-identical plaintext, each put produces a different blob hash because:

1. **Timestamp**: every put uses `time(nullptr)` → different per second
2. **Envelope encryption**: client-side encryption uses fresh random nonces each time → ciphertext differs
3. **ML-DSA-87 signature**: non-deterministic → different blob encoding → different hash

So storage dedup (keyed on encoded-blob hash) doesn't fire for logically-identical content.

**Design options:**

1. **Logical names / overwrite-by-name**: add an optional `--name <key>` flag. Putting the same name tombstones the previous version. Requires a name→hash index on the server and clear ownership/ACL semantics.
2. **`--replace <old_hash>`**: explicit opt-in tombstone-then-put. Keeps immutability as the default.
3. **Dedup by plaintext hash**: client hashes plaintext, sends it as a dedup key; server rejects if already present. Compromises zero-knowledge guarantees depending on implementation.
4. **Do nothing, document the behaviour**: content-addressed + immutable is the correct model; expect callers to check existence first.

**Suggested direction:** #2 as the cleanest short-term answer (explicit user intent), #1 as a longer-term convenience if we want overwrite-by-name as a first-class workflow.

**Related:** Tombstones still propagate via sync (verified), so `--replace`'s delete step would work end-to-end.

Plans:
- [ ] TBD (promote with /gsd-plan-phase when ready)
