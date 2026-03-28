# Phase 69: Documentation Refresh - Research

**Researched:** 2026-03-28
**Domain:** Documentation accuracy and completeness for chromatindb v1.5.0
**Confidence:** HIGH

## Summary

This is a documentation-only phase with zero C++ code changes. Three documentation files need updating: `README.md` (root), `db/README.md`, and `db/PROTOCOL.md`. The primary challenge is **accuracy verification** -- ensuring every number, version string, message type name, byte offset, and wire format in the docs matches the actual source code.

The current documentation is stale at roughly the v1.3.0 level. Since then, v1.4.0 added 18 new message types (9 request/response pairs, enums 41-58), and v1.5.0 added the `dist/` production deployment kit. Neither README mentions the relay, dist/ deployment, or v1.4.0 query features. The PROTOCOL.md at `db/PROTOCOL.md` was already updated during Phase 64 and Phase 67 and appears complete for all 58 types, but must be verified against the encoder source code.

**Primary recommendation:** Split into two plans -- (1) PROTOCOL.md verification against source, (2) README.md + db/README.md refresh with relay/dist sections. Protocol verification is the most tedious and error-prone task; the README updates are straightforward but require gathering exact numbers from the codebase.

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| DOCS-01 | README.md updated with current test/LOC/type counts and v1.4.0 features | Exact current numbers identified: 567 tests, ~29,600 LOC, 58 message types, 49 Docker integration tests. Stale values documented below. |
| DOCS-02 | README.md includes relay deployment section | Relay architecture fully documented in research: config fields, message filter (38 client types), systemd unit ordering. |
| DOCS-03 | README.md includes dist/ deployment instructions | Full dist/ inventory: install.sh, 2 systemd units, 2 configs, sysusers.d, tmpfiles.d. All paths and commands documented. |
| DOCS-04 | PROTOCOL.md verified against source for all 58 message types with correct byte offsets | PROTOCOL.md at db/PROTOCOL.md already covers all 58 types. Verification requires reading peer_manager.cpp encoder sections (lines 700-1600). |
| DOCS-05 | db/README.md updated with current state | Identified all stale sections: version, test count, message type count, missing features, missing config fields. |
</phase_requirements>

## Standard Stack

Not applicable -- this is a documentation phase with no libraries or dependencies to install.

## Architecture Patterns

### Documentation File Layout

```
README.md              # Root overview (currently stale at v1.3.0)
db/README.md           # Detailed technical docs (crypto, arch, config, scenarios)
db/PROTOCOL.md         # Wire protocol reference (byte-level, 828 lines)
dist/                  # Deployment artifacts (Phase 68 output)
  install.sh           # POSIX sh install/uninstall script
  systemd/             # chromatindb.service, chromatindb-relay.service
  config/              # node.json, relay.json
  sysusers.d/          # chromatindb.conf
  tmpfiles.d/          # chromatindb.conf
```

### Documentation Hierarchy

The root `README.md` is a thin pointer to `db/README.md`. It currently shows version ("v1.3.0") and a one-paragraph summary. The actual comprehensive documentation (build, config, wire protocol, scenarios, features) lives in `db/README.md`. The `db/PROTOCOL.md` is a standalone wire format reference for SDK developers.

### Pattern: Source-of-Truth Numbers

Every number in docs must come from a verified source:

| Number | Source of Truth | Current Doc Value | Actual Value |
|--------|----------------|-------------------|--------------|
| Unit test count | `ctest -N` output | 551 | 567 |
| LOC count | `wc -l` on .cpp/.h files (excl. build/) | "~28,000" | ~29,600 |
| Message type count | `TransportMsgType` enum in transport_generated.h | 40 | 58 |
| Docker integration tests | `ls tests/integration/test_*.sh \| wc -l` | 54 | 49 |
| Version | CMakeLists.txt project(VERSION) | v1.3.0 (README), 1.1.0 (CMake) | Should be v1.5.0 |
| Relay allowed types | message_filter.h comment | not in README | 38 |
| Config fields | Config struct in config.h | missing 3 fields | 26 total fields |

**NOTE on Docker integration tests:** The db/README.md says "54 integration tests across 12 categories" but only 49 test scripts exist in `tests/integration/`. The discrepancy may be because some test scripts run multiple sub-tests. The planner must decide: count test scripts (49) or count test scenarios (may be higher). The safest approach is to count scripts and describe them accurately.

**NOTE on version:** CMakeLists.txt says `VERSION 1.1.0`, the latest git tag is `v1.4.0`, and the current milestone is v1.5.0. The version in CMakeLists.txt was frozen at the v1.1.0 milestone (Phase 53) when version injection was last configured. This is a code change (CMakeLists.txt) but the requirements say "zero C++ code changes" -- CMakeLists.txt is not C++ code, it is build configuration. The planner should bump this to 1.5.0.

### Anti-Patterns to Avoid

- **Approximate numbers:** Do not write "over 500 tests" -- write the exact number from ctest.
- **Stale cross-references:** db/README.md references "PROTOCOL.md" via a relative link that expects `db/PROTOCOL.md` -- this works correctly when viewing from db/ but must be verified.
- **Duplicated content between files:** Root README.md and db/README.md should NOT duplicate feature lists. Root is a pointer, db/ is the reference.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Test count | Hand-count TEST_CASE macros | `ctest -N \| tail -1` | Catch2 SECTION macros expand into sub-tests; only ctest knows the real count |
| LOC count | Guess from memory | `find . -name "*.cpp" -o -name "*.h" \| grep -v build \| xargs wc -l` | Must exclude build dir, generated headers |
| Wire format verification | Read PROTOCOL.md and trust it | Compare encoder source in peer_manager.cpp against each documented format | Only way to catch offset/size drift |

## Common Pitfalls

### Pitfall 1: Version Number Inconsistency
**What goes wrong:** Different files show different version numbers (README says v1.3.0, CMakeLists says 1.1.0, memory says v1.5.0).
**Why it happens:** Version was not bumped in CMakeLists.txt after v1.1.0 milestone. README was last fully updated in Phase 64 (v1.3.0).
**How to avoid:** Decide on canonical version (v1.5.0 for this milestone), update CMakeLists.txt, README.md, db/README.md consistently.
**Warning signs:** Any version string that does not match v1.5.0.

### Pitfall 2: Wire Format Documentation Drift
**What goes wrong:** PROTOCOL.md byte offsets or field sizes don't match the actual encoder in peer_manager.cpp.
**Why it happens:** PROTOCOL.md was updated in Phase 67 but encoder code may have been tweaked after the doc was written.
**How to avoid:** Systematic verification: for each message type 41-58, read the encoder code, compute expected payload size, compare against PROTOCOL.md table.
**Warning signs:** A documented payload size that doesn't match `response.size()` in the encoder.

### Pitfall 3: Missing Config Fields in Documentation
**What goes wrong:** db/README.md config section lists fields from v0.9.0 but misses `expiry_scan_interval_seconds` (Phase 54), `compaction_interval_hours` (Phase 55), and `uds_path` (Phase 56).
**Why it happens:** Config reference was not updated when these features were added.
**How to avoid:** Cross-reference Config struct fields in config.h against documented fields in db/README.md.
**Warning signs:** Any field in Config struct not appearing in the configuration documentation.

### Pitfall 4: Incorrect Docker Test Count
**What goes wrong:** Documentation says "54 integration tests" but only 49 test scripts exist.
**Why it happens:** The number may have been correct at some point and tests were reorganized, or the original count included sub-test scenarios within scripts.
**How to avoid:** Count `test_*.sh` files, describe the count accurately.
**Warning signs:** Documented count differs from `ls tests/integration/test_*.sh | wc -l`.

## Stale Documentation Inventory

### README.md (Root)
| Line | Current | Should Be |
|------|---------|-----------|
| 5 | "Current release: v1.3.0" | "Current release: v1.5.0" |
| - | No relay section | Add relay deployment section |
| - | No dist/ section | Add deployment instructions |

### db/README.md
| Section | Current | Should Be |
|---------|---------|-----------|
| Line 64 | "551 unit tests" | "567 unit tests" (or current count at execution time) |
| Line 73 | "54 integration tests across 12 categories" | Verify: 49 test scripts |
| Line 192 | "40 message types" | "58 message types" |
| Config section | Missing expiry_scan_interval_seconds, compaction_interval_hours, uds_path | Add all 3 fields with descriptions |
| Features section | Missing v1.4.0 features (NamespaceList, StorageStatus, NamespaceStats, BlobMetadata, BatchExists, DelegationList, BatchRead, PeerInfo, TimeRange) | Add 9 new query features |
| Features section | Missing request pipelining (v1.3.0 Phase 61) | Add request pipelining feature |
| Features section | Missing ExistsRequest/NodeInfoRequest (v1.3.0 Phase 63) | Already present -- verify |
| - | No relay section | Add relay architecture overview |
| - | No dist/ deployment section | Add deployment with install.sh |

### db/PROTOCOL.md
| Item | Status | Action |
|------|--------|--------|
| Transport layer (lines 1-48) | Current | Verify request_id docs |
| PQ Handshake (lines 49-98) | Current | No changes needed |
| Trusted Handshake (lines 99-118) | Current | No changes needed |
| UDS (lines 119-139) | Current | No changes needed |
| Blob Schema (lines 146-178) | Current | No changes needed |
| Data message (lines 179-191) | Current | No changes needed |
| Sync protocol (lines 193-288) | Current | No changes needed |
| Deletion/Delegation/PubSub (lines 289-361) | Current | No changes needed |
| Storage/Quota/SyncRejected (lines 362-394) | Current | No changes needed |
| Timestamp validation (lines 395-413) | Current | No changes needed |
| Rate limiting (lines 415-418) | Current | No changes needed |
| Inactivity (lines 419-429) | Current | No changes needed |
| Client Protocol types 30-40 (lines 431-552) | Current (Phase 64) | Verify byte offsets against source |
| Message Type Reference table (lines 554-618) | Current (Phase 67) | Verify all 58 entries |
| v1.4.0 Query Extensions types 41-58 (lines 620-828) | Current (Phase 67) | Verify byte offsets against source |

### CMakeLists.txt
| Line | Current | Should Be |
|------|---------|-----------|
| 2 | `project(chromatindb VERSION 1.1.0)` | `project(chromatindb VERSION 1.5.0)` |

## Relay Architecture (for DOCS-02)

The relay is a separate binary (`chromatindb_relay`) that acts as a security boundary between untrusted clients and the chromatindb node.

**Architecture:**
```
Client (TCP, PQ handshake) --> Relay --> (UDS, TrustedHello) --> Node
```

**Key facts for documentation:**
- Relay listens on TCP (default port 4201), connects to node via UDS
- Full PQ handshake (ML-KEM-1024 + ML-DSA-87) with each client
- Default-deny message filter: only 38 client-allowed message types pass through
- Blocked types: all sync, PEX, handshake, reconciliation messages
- Allowed types: Data, WriteAck, Delete, DeleteAck, Read*, List*, Stats*, Exists*, NodeInfo*, Subscribe, Unsubscribe, Notification, Ping, Pong, Goodbye, plus all v1.4.0 query types
- Config: `bind_address`, `bind_port` (4201), `uds_path`, `identity_key_path`, `log_level`, `log_file`
- Relay has its own ML-DSA-87 identity (separate from node)
- Systemd unit: `chromatindb-relay.service`, ordered After=chromatindb.service

## dist/ Deployment (for DOCS-03)

**Install command:**
```bash
sudo dist/install.sh build/db/chromatindb build/relay/chromatindb_relay
```

**Uninstall command:**
```bash
sudo dist/install.sh --uninstall
```

**FHS paths:**
| Artifact | Location |
|----------|----------|
| Node binary | /usr/local/bin/chromatindb |
| Relay binary | /usr/local/bin/chromatindb_relay |
| Node config | /etc/chromatindb/node.json |
| Relay config | /etc/chromatindb/relay.json |
| Node systemd unit | /usr/lib/systemd/system/chromatindb.service |
| Relay systemd unit | /usr/lib/systemd/system/chromatindb-relay.service |
| Data directory | /var/lib/chromatindb |
| Log directory | /var/log/chromatindb |
| Runtime (UDS) | /run/chromatindb |
| System user | chromatindb (via sysusers.d) |

**Post-install:**
```bash
systemctl daemon-reload
systemctl enable --now chromatindb
systemctl enable --now chromatindb-relay
```

**Security:** Both systemd units include 16 hardening directives (ProtectSystem=strict baseline + 13 additional: NoNewPrivileges, MemoryDenyWriteExecute, PrivateTmp, PrivateDevices, ProtectHome, ProtectKernel*, ProtectControlGroups, ProtectClock, ProtectHostname, RestrictRealtime, RestrictSUIDSGID, RestrictNamespaces, LockPersonality, SystemCallArchitectures=native, RestrictAddressFamilies).

**Idempotent:** Running install.sh twice is safe -- configs are preserved (no overwrite), binaries and service files are updated.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | N/A -- documentation-only phase |
| Config file | N/A |
| Quick run command | N/A |
| Full suite command | N/A |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| DOCS-01 | README.md has correct numbers | manual-only | Visual inspection -- grep for exact version/count strings | N/A |
| DOCS-02 | README.md has relay section | manual-only | `grep -c "relay" db/README.md` > 0 | N/A |
| DOCS-03 | README.md has dist/ section | manual-only | `grep -c "install.sh" db/README.md` > 0 | N/A |
| DOCS-04 | PROTOCOL.md byte offsets match source | manual-only | Side-by-side comparison of peer_manager.cpp encoder sections vs PROTOCOL.md tables | N/A |
| DOCS-05 | db/README.md current state | manual-only | Check version string, test count, feature list | N/A |

**Justification for manual-only:** This is a documentation phase. The "tests" are verification that written prose and numbers match source code. There is no behavioral code to unit test.

### Sampling Rate
- **Per task commit:** Visual review of changed documentation sections
- **Per wave merge:** Verify all numbers against source commands
- **Phase gate:** All five success criteria manually verified

### Wave 0 Gaps
None -- no test infrastructure needed for documentation phase.

## Sources

### Primary (HIGH confidence)
- `db/wire/transport_generated.h` -- authoritative enum of all 58 message types (lines 23-84)
- `db/config/config.h` -- authoritative Config struct with all 26 fields
- `relay/config/relay_config.h` -- authoritative RelayConfig struct with all 6 fields
- `relay/core/message_filter.h` -- authoritative list of 38 client-allowed types
- `db/peer/peer_manager.cpp` (3490 lines) -- authoritative encoder source for all response wire formats
- `db/PROTOCOL.md` (828 lines) -- current protocol documentation
- `dist/` directory -- Phase 68 output (install.sh, systemd units, configs, sysusers.d, tmpfiles.d)
- `ctest -N` output -- 567 registered tests
- `wc -l` on source files -- ~29,600 LOC
- `tests/integration/test_*.sh` -- 49 integration test scripts

### Secondary (MEDIUM confidence)
- `.planning/PROJECT.md` -- project state and feature history
- `.planning/REQUIREMENTS.md` -- requirement definitions
- `.planning/ROADMAP.md` -- milestone/phase history

## Metadata

**Confidence breakdown:**
- Documentation inventory: HIGH -- all three doc files fully read and compared against source
- Current numbers: HIGH -- verified via ctest, wc -l, enum inspection, file listing
- Wire format accuracy: MEDIUM -- PROTOCOL.md was updated in Phase 67 but byte-level verification against encoder source not yet performed (that is part of the plan execution)
- Relay/dist content: HIGH -- source files read directly

**Research date:** 2026-03-28
**Valid until:** Indefinite -- numbers may drift if code changes occur before this phase executes, but v1.5.0 is documented as "zero C++ code changes"
