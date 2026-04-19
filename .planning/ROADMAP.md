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
- [x] **Phase 119: Chunked Large Files** - Upload/download files >500 MiB via CDAT chunks + CPAR manifest with envelope v2 truncation prevention (completed 2026-04-19)
- [x] **Phase 120: Request Pipelining** - Multi-blob pipelined downloads over single PQ connection (completed 2026-04-19)
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
**Depends on**: Phase 120 (pipelining makes chunked transfers near wire-speed instead of RTT-bound)
**Requirements**: CHUNK-01, CHUNK-02, CHUNK-03, CHUNK-04, CHUNK-05
**Success Criteria** (what must be TRUE):
  1. User can upload a file >500 MiB and the CLI streams it in chunks without buffering the entire file in memory
  2. Upload produces CDAT chunk blobs plus a CPAR manifest blob that references all chunks
  3. Download of a CPAR manifest automatically reassembles all referenced chunks into the original file
  4. `cdb rm` of a manifest blob deletes the manifest and all associated CDAT chunk blobs
  5. Envelope format v2 includes segment count, preventing a truncation attack where an attacker drops trailing chunks
**Plans**: 3 plans
Plans:
- [x] 119-01-PLAN.md — Chunked upload (put_chunked), cascade delete (rm_chunked), manifest codec, Sha3Hasher, CPAR magic, cmd::put/cmd::rm dispatch
- [x] 119-02-PLAN.md — Chunked download (get_chunked), cmd::get dispatch, post-reassembly SHA3 verify
- [x] 119-03-PLAN.md — Gap closure: CR-01 in_flight leak (pump_recv_any + recv_next + 5 call-site migrations) + WR-02/WR-03/IN-03 + live-node E2E gate

### Phase 120: Request Pipelining
**Goal**: Multi-blob downloads (and uploads) complete faster by pipelining requests over a single PQ connection instead of sequential round-trips
**Depends on**: Phase 117 (stable ListResponse wire format)
**Requirements**: PIPE-01, PIPE-02, PIPE-03
**Success Criteria** (what must be TRUE):
  1. Downloading multiple blobs uses pipelined requests over a single PQ-encrypted connection (no new handshake per blob)
  2. Single reader thread invariant is maintained -- no concurrent recv on the same connection (AEAD nonce desync prevention)
  3. Pipeline depth is configurable with a sensible default of 8 in-flight requests
**Plans**: 2 plans
Plans:
- [x] 120-01-PLAN.md — Connection::send_async + recv_for + correlation map + Catch2 [pipeline] tests
- [x] 120-02-PLAN.md — Pipeline cmd::get and cmd::put onto send_async + arrival-order recv() drain

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
116 -> 117 -> 118 -> 120 -> 119 -> 121 -> 122

Phase 120 (pipelining) is now executed before 119 (chunked large files) so that
chunked uploads/downloads inherit pipelined request handling. Dependency was
flipped 2026-04-18: 120 depends only on 117's stable wire format; 119 depends
on 120 so the big-file path lands with near-wire-speed transfers from day one.

Note: Phase 118 depends only on Phase 116 (not 117), so it could execute in parallel with Phase 117 if desired.

| Phase | Plans Complete | Status | Completed |
|-------|----------------|--------|-----------|
| 116. CLI Rename + Contact Groups | 2/2 | Complete    | 2026-04-16 |
| 117. Blob Type Indexing + ls Filtering | 2/2 | Complete    | 2026-04-16 |
| 118. Configurable Constants + Peer Management | 2/2 | Complete    | 2026-04-16 |
| 119. Chunked Large Files | 3/3 | Complete    | 2026-04-19 |
| 120. Request Pipelining | 2/2 | Complete   | 2026-04-19 |
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

**Plans:** 3/3 plans complete

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


### Phase 999.3: Determine if we want per-namespace R / W / RW access modes (BACKLOG — research)

**Goal:** Decide whether the node's ACL model should support per-namespace access modes (read-only, write-only, read-write) instead of the current coarser "access allowed for everything that passes ACL" semantics.

**Requirements:** TBD — this phase is a design decision, not a build.

**Plans:** 0 plans

#### Current model

- Node has two ACLs: `allowed_client_keys` and `allowed_peer_keys`. Membership is binary — allowed or not.
- Once a peer is allowed, they can receive any sync for any namespace the node holds (modulo `sync_namespaces` filter, which is node-wide, not per-peer).
- Once a client is allowed, they can read/write/delete any blob (subject to signature checks — writers must own the namespace key or be a delegate).
- Delegation (`cdb delegate` / `cdb revoke`) grants write access to other identities within a namespace. There's no R vs W distinction in delegations today; a delegate has full write rights.

#### What the proposal would add

Per-namespace capability ACL:
- **R**: can receive sync / fetch blobs from this namespace but cannot publish
- **W**: can publish blobs to this namespace but cannot receive sync back
- **RW**: current default — both

Possible scopes where this applies:
1. **Per-peer**: per-peer namespace filter with an R/W flag. A remote peer can mirror a namespace read-only, or act as an offsite write-target without being allowed to read back.
2. **Per-client**: per-client per-namespace caps. Some clients are read-only (query-only tools), others are write-only (drop-box uploaders).
3. **Per-delegation**: differentiate `cdb delegate --read` vs `cdb delegate --write`. Today delegate means W.

#### Design questions to answer

- Is there a real use case users are hitting today, or is this speculative? (Enterprise storage vault direction from `project_product_direction.md` might want this; LAN sync probably doesn't need it.)
- Where does it live: in the ACL config (static) or in delegations (dynamic, signed)?
- How does a write-only namespace interact with sync? Is the peer sending hashes (metadata read) acceptable, or does that count as "read"?
- Does enforcement happen at dispatch time (node rejects ops it doesn't permit) or at handshake time (node refuses to even connect a peer with bad caps)?
- What's the wire impact? New allow-list fields in config.json? New fields in `SyncNamespaceAnnounce`? Both?
- Relationship to tombstones: write-only peers should still be able to send tombstones (otherwise deletion propagation breaks) — but that means they effectively have metadata-read on the hashes they tombstone. Worth thinking through.

#### Suggested direction (to revisit in discussion)

Start by gathering concrete use cases before committing to implementation. If the only motivation is theoretical completeness, punt; if there are real deployments that need it, design per-peer R/W as the first pass since that's the sync-boundary use case. Delegation R/W is a separate decision with different implications (who signs what).

Plans:
- [ ] TBD (promote with /gsd-discuss-phase when ready to research)


### Phase 999.4: cdb regression test suite (BACKLOG)

**Goal:** End-to-end Catch2 tests that exercise the `cdb` binary against a local test node so the sweep's worth of fixes can't silently regress.

**Requirements:** TBD

**Plans:** 0 plans

Surface this sweep found 20 independent UX/correctness bugs in `cdb`, one of which (B1 — the 40-byte ListResponse stride against a 44-byte server wire format) completely broke `contact add` and the `put --share @group` workflow. `cli/tests` exists in the tree but the post-sweep behavior has no coverage. A careless refactor could reintroduce any of:

- stride mismatches on ListResponse consumers (the class of bug behind B1)
- positional `host[:port]` re-appearing and silently overriding `--node`
- `rm` losing its target-existence pre-check
- `get --force` / `rm --force` scope regressions
- error messages drifting away from the unified `Error: …` convention
- 0-byte / directory / non-existent-file put paths

**Suggested direction:**

1. Add a Catch2 fixture that launches a fresh `chromatindb` on a random port + scratch datadir, waits for it to listen, tears it down in the destructor.
2. Cover the full sweep matrix: identity/info, blob lifecycle (put/get/exists/rm + --force + empty/dir rejections), contacts/groups/sharing (including `put --share @group` end-to-end which is the workflow B1 killed), delegate/revoke with contact-name + @group + file, cross-node sync via a second fixture instance.
3. Invoke the built `cdb` binary as a subprocess (not its internal functions) so argv parsing, stdout/stderr separation, and exit codes are all in scope — these are where most sweep findings lived.
4. Wire into the existing CTest setup so it runs in the same target as the node's tests.

**Depends on:** none

Plans:
- [ ] TBD (promote with /gsd-plan-phase when ready to build)


### Phase 999.5: cdb get handles PUBK (and non-envelope) blobs gracefully (BACKLOG)

**Goal:** `cdb get <pubk_hash>` (or `get --all --from <self-ns>`) should say "this blob isn't a CENV envelope, nothing to decrypt" instead of `"Metadata length exceeds payload"`.

**Requirements:** TBD

**Plans:** 0 plans

Current behaviour (observed during final sweep): `cdb get --all --from self` happily fetches the PUBK blob in its own namespace, then fails decoding because PUBK data isn't a CENV envelope with a length-prefixed metadata section. The error surfaces as `<hash>: Metadata length exceeds payload` — technically accurate but completely opaque to a user who just asked for "all my blobs".

**Suggested direction:**

1. In `get()`, after fetching a blob, inspect the type magic before attempting envelope decrypt / payload parse. For PUBK/TOMB/DLGT/CDAT (chunk), emit a structured message ("hash <h>: type PUBK, not user data") and skip — don't count as an error when combined with `--all`.
2. Consider adding a `--type` filter to `get --all` that mirrors `ls --type` so the caller can opt into strict selection.
3. For direct `get <hash>`, still error out (the user asked specifically) but with a clearer message than "Metadata length exceeds payload".

**Depends on:** none

Plans:
- [ ] TBD (promote with /gsd-plan-phase when ready to build)


### Phase 999.6: cdb contact add — alias dedup / warn on namespace collision (BACKLOG)

**Goal:** Decide and enforce whether `cdb contact add alice <ns>` and `cdb contact add bob <ns>` pointing at the same namespace is intentional (aliasing) or accidental (typo) — and surface it either way.

**Requirements:** TBD

**Plans:** 0 plans

Observed during final sweep: adding two distinct names for the same namespace succeeds silently. This is either:

- **Aliasing** — reasonable, a user might want `alice` and `alice-work` to point at the same key. In that case it should at least warn the first time.
- **Accident** — user pasted the same namespace twice under different names; should be caught.

**Suggested direction:**

1. On `contact add`, query existing contacts for the same namespace. If any exist, print a one-line note to stderr: `"note: namespace already known as <existing-name>"` — informational, not blocking.
2. Add `contact add --allow-duplicate-ns` if we want to silence the note explicitly.
3. Consider a `contact resolve <ns>` subcommand that prints all names pointing at a namespace (handy for audit after delegate/revoke).

**Depends on:** none

Plans:
- [ ] TBD (promote with /gsd-plan-phase when ready to build)


### Phase 999.7: cdb shell completion + man page (BACKLOG)

**Goal:** Ship bash/zsh completion scripts and a proper `man cdb` page so the CLI feels like a first-class Unix tool.

**Requirements:** TBD

**Plans:** 0 plans

Current state: every flag and subcommand is discoverable only by running `cdb --help` or the command with no args. No tab completion for subcommands, contact names, `@group` expansion, or hashes. No man page at all.

**Suggested direction:**

1. Generate completion scripts for bash and zsh that:
   - Complete subcommand names at position 1
   - Complete flags per-subcommand (e.g. `--share`, `--ttl`, `--type`)
   - Dynamically complete contact names and group names by reading `~/.cdb/contacts.db` (via a helper `cdb contact list` in machine-readable mode)
   - Complete `--node` with names from `~/.cdb/config.json`
2. Author a single `cdb.1` man page. Keep it scannable — synopsis per subcommand, a "FILES" section pointing at `~/.cdb/`, an "EXAMPLES" section covering the enterprise workflow (`publish` → `contact add` → `put --share @team` → `get`).
3. Install target: completions under `/usr/share/bash-completion/completions/cdb` and zsh equivalent; man page under `/usr/share/man/man1/cdb.1`.
4. Probably warrants a `cdb contact list --format json` or similar machine-readable mode for the dynamic completion to consume cleanly.

**Depends on:** none

Plans:
- [ ] TBD (promote with /gsd-plan-phase when ready to build)


### Phase 999.8: Bounded PQ handshake timeout on the cdb side (BACKLOG)

**Goal:** `cdb <cmd>` against a node that accepts TCP but stalls mid-PQ-handshake should fail within a bounded window (same class as the 5s TCP connect timeout we just added), not hang indefinitely.

**Requirements:** TBD

**Plans:** 0 plans

M5 in the sweep added a 5s TCP connect timeout via `async_connect` + `steady_timer` in `Connection::connect_tcp`. That covers the "dead host / SYN timeout" case. But the handshake path (`handshake_pq` and `handshake_trusted`) uses synchronous `asio::read` calls with no deadline — if a node completes the TCP handshake but then never sends the KEM pubkey (or gets stuck mid-exchange), `cdb` blocks forever. The original sweep's backlog 999.2 observation about `cdb ls` stalling on handshake called this out.

**Suggested direction:**

1. Mirror the connect_tcp pattern: rewrap `asio::read` / `asio::write` in the handshake path with `async_read` / `async_write` + a per-phase deadline (maybe 10s total for PQ handshake).
2. On timeout, close the socket and return a clear error: `"Error: PQ handshake timed out with <host>:<port> — is the node version compatible?"`
3. Also consider a shorter deadline on `handshake_trusted` (UDS, same-host) since that should complete in milliseconds.
4. Verify against a contrived "hang after TCP accept" test server (easy to build in a few lines of asio).

**Depends on:** none

Plans:
- [ ] TBD (promote with /gsd-plan-phase when ready to build)


### Phase 999.9: Verify export-key TTY refusal in a real terminal (BACKLOG)

**Goal:** Confirm the `export-key` default format guard (refuse to splatter raw 4160 bytes onto a TTY, suggest `--out` / `--format hex|b64`) actually triggers when run interactively.

**Requirements:** TBD

**Plans:** 0 plans

The M4 sweep fix added `isatty(fileno(stdout))` + a stderr hint that refuses raw binary when stdout is a terminal. The logic is straightforward, but it couldn't be exercised through the Bash tool during the sweep because the tool captures stdout over a pipe (so `isatty` returns 0 and the guard is bypassed). The guard's correctness was asserted by reading the code, not by observation.

**Suggested direction:**

1. Open a real terminal, run `cdb export-key` with no flags, confirm the stderr message fires and no binary is emitted.
2. Verify the escape hatches: `cdb export-key > file.pub` writes 4160 bytes without complaint; `cdb export-key --format hex` prints hex to the TTY without complaint; `cdb export-key --out file.pub` writes to file.
3. If anything doesn't behave, tighten the check (e.g. also look at `isatty(fileno(stderr))` to avoid pathological redirections).
4. Consider adding an automated test that uses a pty via `openpty(3)` so this never has to be a manual step again.

**Depends on:** none

Plans:
- [ ] TBD (promote with /gsd-plan-phase when ready to build)

### Phase 999.10: Phase 119 chunked-file code review cleanup (BACKLOG)

**Goal:** Close the 3 warning + 5 info findings from the post-119-03 code review that were deferred as non-blocking cleanup. None are observed in live testing; each is a correctness or legibility refinement to the chunked-file codepath.

**Requirements:** TBD

**Plans:** 0 plans

Full findings in `.planning/phases/119-chunked-large-files/119-REVIEW.md`.

**Warnings (3):**

1. **WR-01 — `put_chunked` retry can send different plaintext than was hashed** (`cli/src/chunked.cpp:220-249`)
   The first read feeds `hasher.absorb` before `send_async`; a retry re-reads from disk and sends whatever the file contains now, so the CDAT on the wire can diverge from `manifest.plaintext_sha3`. A subsequent `cdb get` then fails the final SHA3 check even though every chunk was received intact. Secondary: a retry with `got == 0` silently sends an empty plaintext as a valid signed CDAT. **Fix direction:** hash AFTER successful send, or reject retries where re-read size differs from first read (and reject `got == 0`).

2. **WR-02 — `pump_recv_any` decrements `in_flight_` on unknown-rid replies** (`cli/src/pipeline_pump.h:126-143` + 5 caller sites)
   Every returned message decrements `in_flight_`, including stray/duplicate replies the caller discards. Over time the pipeline window ceiling drifts above `kPipelineDepth`. Well-behaved nodes don't trigger this; it's defense-in-depth hardening. **Fix direction:** either add `Connection::credit_in_flight()` and have callers re-credit on unknown-rid, or move the decrement into the caller on a known-rid match (matches `pump_recv_for` semantics).

3. **WR-03 — `decode_manifest_payload` lacks lower bound on `total_plaintext_bytes`** (`cli/src/wire.cpp:396`)
   A manifest with large `segment_count` but tiny `total_plaintext_bytes` causes `expected_len = total - off` to underflow in `get_chunked:691-700`. The `pt->size() != expected_len` check still catches it (no memory corruption), but the error message misdirects. **Fix direction:** add `min_plain = (segment_count - 1) * chunk_size_bytes + 1` check during manifest decode.

**Info (5):**

4. **IN-01 — `rm_chunked` has no retry policy for tombstone sends** (`cli/src/chunked.cpp:391-403`)
   `put_chunked` and `get_chunked` retry with the D-15 backoff ladder (250/1000/4000 ms). `rm_chunked` doesn't — a single dropped packet during tombstone fan-out leaves a chunk un-tombstoned and forces a full idempotent retry. **Fix direction:** mirror the `put_chunked` retry pattern, or explicitly document that D-10 idempotency is the sole recovery strategy.

5. **IN-02 — Magic `41` WriteAck/DeleteAck threshold repeated at 5 sites** (`cli/src/chunked.cpp:283,338,426,452` + `cli/src/commands.cpp:653`)
   `41 = hash:32 + seq:8BE + status:1`. Violates the "never copy-paste utilities" project-memory rule. **Fix direction:** extract `WRITE_ACK_MIN_SIZE` / `DELETE_ACK_MIN_SIZE` constants in `cli/src/wire.h`, replace all 5 sites.

6. **IN-03 — `%llu` printf formats for `uint64_t`** (5 sites in `cli/src/chunked.cpp`)
   Casts `uint64_t` → `unsigned long long` to satisfy `%llu`. Works on Linux x86_64 but verbose. **Fix direction:** adopt `PRIu64` from `<cinttypes>` or migrate to spdlog's native `uint64_t` formatters.

7. **IN-04 — `Sha3Hasher::finalize` lacks double-call guard** (`cli/src/wire.cpp:313-333` + `cli/src/wire.h:179-190`)
   The header documents "must be called exactly once" and `Impl::finalized` tracks state, but `finalize()` itself never checks. A second call would invoke `OQS_SHA3_sha3_256_inc_finalize` on a released context. Latent — all current callers call once — but cheap to enforce. **Fix direction:** `if (impl_->finalized) throw std::logic_error(...)` at the top of `finalize()`.

8. **IN-05 — `put_chunked` stdin path is unreachable dead code** (`cli/src/chunked.cpp:141-168`)
   `cmd::put` only dispatches to `put_chunked` for regular files (caps stdin at `MAX_FILE_SIZE` without going through chunked). The `filename.empty()` → `"<stdin>"` fallback at `chunked.cpp:352-353` can never fire. **Fix direction:** remove the `filename.empty()` handling, or add a comment stating `path` is always a regular file.

**Depends on:** none (the live-node end-to-end path already works; these are hardening/cleanup)

Plans:
- [ ] TBD (promote with /gsd-plan-phase when ready to build)

### Phase 999.11: Schema + signing cleanup — strip namespace_id, compress pubkey, mandatory PUBK (BACKLOG — MVP-gating, protocol-breaking)

**Goal:** One coordinated breaking change to the blob wire format and signing canonical form that shrinks every signed blob by ~2592 bytes (~35%) and removes a redundant field from the schema before we freeze it for v1.

**Requirements:** TBD

**Plans:** 0 plans

**Motivation:** Pre-MVP audit found that `Blob.namespace_id` is always `SHA3(pubkey)` and therefore derivable on every verify path. `pubkey` (2592 bytes) is embedded in every blob when it's really per-namespace-owner and could live in a node-local index once published. Both were keeping the schema bigger than needed. This is the final window to make breaking changes cleanly.

**What changes:**

1. **`db/schemas/blob.fbs`** — drop `namespace_id:[ubyte]` field (saves 32 bytes/blob). Replace `pubkey:[ubyte] (required)` (2592 bytes) with `signer_hint:[ubyte]` (32 bytes) — points into either the `owner_pubkeys` DBI or the existing `delegation_map`.

2. **Signing canonical form** — change from `SHA3-256(namespace_id || data || ttl || timestamp)` to either `SHA3-256(SHA3-256(pubkey) || data || ttl || timestamp)` (namespace derived inline) or `SHA3-256(pubkey || data || ttl || timestamp)` (one fewer hash, same security). Pick one and document.

3. **Node ingest** — compute `derived_ns = SHA3(pubkey)` once per ingest, use as namespace for indexing. Remove the `derived_ns == blob.namespace_id` sanity check (engine.cpp:190-193) — no longer meaningful.

4. **New `owner_pubkeys` DBI** — `[ns:32] → pubkey:2592`. Populated the moment a PUBK blob is ingested for a namespace.

5. **Mandatory PUBK-first rule** — node rejects any write to a namespace unless a PUBK blob has already been ingested for it. This makes `signer_hint` → `pubkey` lookup always succeed. Adds ~7 KB one-time cost per namespace, saves ~2560 bytes on every subsequent write.

6. **Verify flow** — receive blob → look up `signer_pubkey` via `signer_hint` (owner_pubkeys DBI for owner writes, delegation_map for delegate writes) → verify ML-DSA-87 signature under that pubkey → proceed.

7. **`cdb publish` is no longer optional** — CLI must auto-publish PUBK on first namespace write. See 999.20.

**Depends on:** none (but 999.12, 999.19, 999.20 all layer on top of this)

Plans:
- [ ] TBD (promote with /gsd-plan-phase when ready to build)

### Phase 999.12: Tombstone batching + name-tagged overwrite (BACKLOG — MVP-gating, client-only)

**Goal:** Ship the overwrite semantic using new blob magics entirely client-side, and shrink tombstone bloat 200–300× by batching multiple deletes under one signature.

**Requirements:** TBD

**Plans:** 0 plans

**Motivation:** The node is intentionally dumb — anything a client can express as a signed blob with a new magic, the node stores and syncs for free. That means we can add mutable-name semantics (`cdb put --name foo`) and batched tombstones without changing the node, then have the node opt-in to understand the new magics for a sync/storage efficiency bonus later.

**What changes:**

1. **New `NAME` magic (`0x4E414D45`)** — payload: `[magic:4][name_len:2][name:N][target_content_hash:32]`. A NAME blob is a signed pointer from a name-in-a-namespace to a content-addressed blob.

2. **New `BOMB` magic (batched tombstone)** — payload: `[magic:4][count:4][target_hash_0:32]...[target_hash_N:32]`. One signature amortized across up to 256 deletes per batch.

3. **`cdb put --name foo file.bin`** — writes a normal content-addressed blob, then writes a NAME blob tagging it. Returns `foo` as the logical handle.

4. **`cdb get foo`** — client enumerates NAME blobs in the namespace via `ls` filtered by NAME magic, picks highest `seq_num`, follows `target_content_hash`. Maintains a local `~/.chromatindb/name_cache.json` keyed by `(ns_hex, name)` → `(seq, target_hash)` for fast lookup without the full enumeration each time.

5. **Overwrite semantics** — writing `--name foo` again emits a new NAME blob with a higher seq and (optionally) a BOMB blob tombstoning the prior NAME + prior content blob. Writer controls cleanup.

6. **Batched delete** — CLI accumulates pending tombstones for N seconds or K deletes, then emits one BOMB blob. Per-tombstone on-wire cost drops from ~7 KB (individual tombstone) to ~50 bytes amortized.

7. **Seq-based tombstone variant (optional optimization)** — BOMB payload can alternatively carry `[seq_be:8]` per dead entry instead of `[target_hash:32]`, 4× smaller. Requires receiver to resolve seq→hash via local sequence DBI on ingest. Writer picks whichever is denser for the batch.

8. **Later node opt-in** — node learns NAME magic and maintains a derived `name_map` DBI for fast lookup, learns BOMB magic and updates `tombstone_map` from batch entries for sync-time filtering. Optional, non-breaking.

**Depends on:** 999.11 (for compressed pubkey — affects BOMB/NAME blob sizes)

Plans:
- [ ] TBD (promote with /gsd-plan-phase when ready to build)

### Phase 999.13: Storage concurrency invariant — verify + document (BACKLOG — MVP-gating, possibly invasive)

**Goal:** Prove or fix the thread-safety invariant in `db/storage/`. Either add an explicit ASIO strand confining all storage access to one thread, or find and document the existing mechanism that makes concurrent ingests safe.

**Requirements:** TBD

**Plans:** 0 plans

**Motivation:** `db/storage/storage.h:104` declares storage is NOT thread-safe. `db/engine/engine.cpp:105-339` calls `store_blob()` inside a co_await. The pre-MVP audit could not prove from code-reading alone that storage is protected from concurrent access under multi-connection ingest. If the protection is implicit (e.g., "only one io_context thread") it needs to be documented at the site where it's relied on; if there's no protection, this is a latent data-corruption bug.

**Investigation steps:**

1. Trace every call path into `store_blob()`, `delete_blob()`, `store_cursor()`, and friends. Identify what thread each is invoked from.
2. Check if there's one strand per engine or if storage is reached from multiple strands.
3. Look for `std::mutex` or `asio::strand` members on `Engine`, `Storage`, or `PeerManager`.
4. If multiple connections can dispatch to storage concurrently, add an explicit strand (or a single storage-worker strand) before MVP.
5. Add a comment at `Storage::store_blob` citing the guarantee mechanism.

**Depends on:** none

Plans:
- [ ] TBD (promote with /gsd-plan-phase when ready to build)

### Phase 999.14: Dispatcher wire-size table (BACKLOG — internal, non-breaking)

**Goal:** Centralize the min/max payload size checks scattered across `db/peer/message_dispatcher.cpp` into a single `db/wire/message_sizes.h` table and helper, so schema changes can't silently desync from dispatcher validation.

**Requirements:** TBD

**Plans:** 0 plans

**Motivation:** Project memory rule: "Wire format payload sizes must match node's dispatcher checks exactly" — this rule exists because it was violated before. Today `message_dispatcher.cpp` carries hardcoded constants (`if (payload.size() < 64)` etc.) at every routing branch. Adding a new message type or changing a size requires a grep; nothing enforces consistency.

**What changes:**

1. New header `db/wire/message_sizes.h` with a `constexpr` struct: `{MsgType, min_size, max_size, label}` entries for every message type.
2. Dispatcher helper `validate_payload_size(msg_type, payload)` returning `{ok, error_code, log_line}`.
3. Dispatcher routing calls the helper once per message, at the top of each handler.
4. Remove hardcoded `payload.size() <` checks at all call sites, replace with the helper.
5. Unit test asserting every MsgType in the enum has a corresponding table entry.

**Depends on:** none (safe to do in parallel with 999.11)

Plans:
- [ ] TBD (promote with /gsd-plan-phase when ready to build)

### Phase 999.15: Sync uses Phase 120 pipelining primitive (BACKLOG — internal, non-breaking)

**Goal:** Node-side sync orchestrator adopts the pipelined `send_async` primitive added in Phase 120 so reconciliation and blob transfer run at the kPipelineDepth=8 window instead of strictly sequential per-peer.

**Requirements:** TBD

**Plans:** 0 plans

**Motivation:** Phase 120 introduced `Connection::send_async` + `recv_next` on the CLI side and node side, but `db/peer/sync_orchestrator.cpp` still sends requests one at a time within a sync round. On high-latency links (real-world peers, not loopback) this caps throughput at 1/RTT per round instead of 8/RTT. No protocol change — just node-side code adopting an existing primitive.

**What changes:**

1. Replace sequential `co_await conn.send(req)` + `conn.recv()` pairs in sync with the pipelined form.
2. Reuse the `pipeline::pump_recv_any` helper (added in Phase 119-03) to drain replies in arrival order.
3. Maintain `in_flight_` bookkeeping the same way CLI does.

**Depends on:** 999.13 (because this touches the concurrency story)

Plans:
- [ ] TBD (promote with /gsd-plan-phase when ready to build)

### Phase 999.16: Cursor DBI periodic GC (BACKLOG — internal, non-breaking)

**Goal:** Run `cleanup_stale_cursors()` periodically during node lifetime, not only at startup, so cursors for peers that briefly connect and vanish (network partitions, crash-restart peers) don't accumulate forever.

**Requirements:** TBD

**Plans:** 0 plans

**Motivation:** `db/storage/storage.cpp:303-304` calls `cleanup_stale_cursors()` at startup. Each cursor is `[peer_hash:32][namespace:32] → 20 bytes`, so ~64 bytes per (peer, namespace) pair. For a stable deployment with known peers this is fine. Under churn (transient peers, P2P bootstrap scenarios) it grows monotonically until the next restart.

**What changes:**

1. Hook `cleanup_stale_cursors()` into the existing expiry-scan background loop (`sync_orchestrator::expiry_scan_loop`), firing every N rounds or every M seconds (config knob).
2. Pass the current known-peer set from `PeerManager` to the cleanup function so it doesn't delete cursors for active peers.
3. Log `info` level: `cursor gc: purged X entries for Y dead peers`.

**Depends on:** none

Plans:
- [ ] TBD (promote with /gsd-plan-phase when ready to build)

### Phase 999.17: SHA3(pubkey) → namespace LRU cache (BACKLOG — internal, non-breaking, optional)

**Goal:** Small in-memory LRU mapping `pubkey_sha3_prefix → namespace_id` so repeated ingests from the same peer skip redundant SHA3 computation on the 2592-byte pubkey.

**Requirements:** TBD

**Plans:** 0 plans

**Motivation:** `db/engine/engine.cpp:189-192` offloads `SHA3(pubkey)` to the thread pool on every ingest. SHA3 on 2592 bytes is sub-millisecond but represents the majority of cheap-path work when ingesting from a high-volume peer. An 8-byte prefix key + 32-byte value = 40 bytes per cache entry; a 1024-entry cache is 40 KB.

**What changes:**

1. New `db/util/pubkey_namespace_cache.h` — lock-free LRU (or strand-confined if lock-free is overkill).
2. Engine ingest: check cache first, fall through to SHA3 compute on miss, populate on success.
3. Invalidation: none needed — namespace is mathematically derived and stable.

**Depends on:** 999.11 (less relevant after pubkey compression because the hint itself replaces the hash in most paths, but still useful for full-pubkey blobs)

Plans:
- [ ] TBD (promote with /gsd-plan-phase when ready to build)

### Phase 999.18: Signature verify thread pool sizing (BACKLOG — config check, trivial)

**Goal:** Confirm the ingest thread pool has >1 worker and is sized proportional to expected concurrent ingest load, so ML-DSA-87 signature verifies (~5-10 ms each) parallelize across cores.

**Requirements:** TBD

**Plans:** 0 plans

**Motivation:** `db/engine/engine.cpp:263-277` offloads each verify to a thread pool. If the pool has 1 worker, a burst of 100 ingests serializes through ~1 second of verify time. Check the pool configuration and either raise the default or document why the current size is right for MVP.

**What changes:**

1. Find where the ingest thread pool is constructed.
2. Verify the default size (likely `std::thread::hardware_concurrency()`).
3. Add a config knob `node.ingest_pool_size` if missing; default to `hardware_concurrency()`.
4. Document the tuning guidance in `db/engine/engine.h` or README.

**Depends on:** none

Plans:
- [ ] TBD (promote with /gsd-plan-phase when ready to build)

### Phase 999.19: Documentation update for MVP protocol (BACKLOG — MVP-gating, MUST-DO)

**Goal:** Update PROTOCOL.md, README.md, cli/README.md, and every other document that describes the wire format, signing canonical form, or storage model to exactly match the post-999.11 + post-999.12 reality. Any doc that contradicts the new protocol is a bug.

**Requirements:** TBD

**Plans:** 0 plans

**Motivation:** User requirement — documentation MUST reflect the new design before MVP ships. MVP without accurate docs is not MVP.

**Scope (non-exhaustive):**

1. `PROTOCOL.md` — full rewrite of the Blob section, signing canonical form section, namespace derivation section. New sections: PUBK-first rule, owner_pubkeys DBI, signer_hint semantics, NAME + BOMB magics (if 999.12 ships).
2. `README.md` — user-facing feature description, updated to mention chunked files, pipelining, mutable names (if 999.12 ships).
3. `cli/README.md` — updated command reference: `cdb publish` is now automatic, `cdb put --name`, `cdb get <name>`, `cdb rm` handles batched tombstones transparently.
4. `db/ARCHITECTURE.md` if it exists, otherwise create it — document the 8 DBIs, the ingest pipeline, the sync protocol, the strand/concurrency model (post-999.13).
5. Update any inline code comments that describe the OLD wire format or signing model (grep for `namespace_id`, `pubkey:2592`, `SHA3(namespace || data`).
6. Remove or archive any doc section describing removed fields (`namespace_id` in wire format, per-blob pubkey embedding).

**Depends on:** 999.11, 999.12, 999.13 (docs must describe the final wire format and invariants)

Plans:
- [ ] TBD (promote with /gsd-plan-phase when ready to build)

### Phase 999.20: CLI adapted to new MVP protocol (BACKLOG — MVP-gating, MUST-DO)

**Goal:** Update the `cdb` CLI to generate blobs in the new wire format (signer_hint instead of inline pubkey, new signing canonical form), enforce the PUBK-first rule on first-write, emit NAME-tagged blobs for `--name` flags, and emit BOMB batched tombstones.

**Requirements:** TBD

**Plans:** 0 plans

**Motivation:** After 999.11 + 999.12 land on the node, the current CLI will be fully broken against the new protocol — it still embeds the full pubkey, uses the old signing canonical form, and writes individual tombstones. Entire CLI blob-encoding and signing paths need updating.

**Scope:**

1. **Identity / key management** — first run of `cdb` (or first write to a fresh namespace) auto-publishes a PUBK blob before anything else. `cdb publish` as a manual command can remain but is no longer required.
2. **Blob encoding** (`cli/src/wire.h`, `cli/src/wire.cpp`) — new `build_blob()` emits signer_hint instead of full pubkey. Old inline-pubkey path deleted outright.
3. **Signing canonical form** — `build_signing_input()` matches the new `SHA3(pubkey || data || ttl || timestamp)` (or whichever form 999.11 picks).
4. **Delegation write path** — delegate's CLI uses `SHA3(delegate_pubkey)` as signer_hint; node looks up via delegation_map.
5. **`cdb put --name foo file`** — emits a NAME magic blob after the content blob (if 999.12 ships).
6. **`cdb get <name>`** — local name_map cache + NAME blob enumeration fallback.
7. **`cdb rm`** — aggregates pending tombstones, emits one BOMB blob per batch.
8. **Backward compatibility** — none. Per project memory rule `feedback_no_backward_compat.md`, old CLI and new CLI are incompatible; only the new one is supported post-MVP.
9. **Regression tests** — every existing CLI Catch2 test re-verified against new wire format. New tests for PUBK-first auto-publish, NAME, BOMB.
10. **Live-node verification** — full put → get → rm → ls round trip against the live node at 192.168.1.73 after the node is running 999.11+999.12 builds.

**Depends on:** 999.11, 999.12 (node protocol defined first)

Plans:
- [ ] TBD (promote with /gsd-plan-phase when ready to build)
