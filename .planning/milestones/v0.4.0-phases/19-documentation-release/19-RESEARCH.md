# Phase 19: Documentation & Release - Research

**Researched:** 2026-03-12
**Domain:** Technical documentation, version management
**Confidence:** HIGH

## Summary

Phase 19 is a documentation and release phase with three deliverables: (1) relocate and expand the README from root to `db/README.md` as the comprehensive operator reference, (2) create a protocol walkthrough document showing how to connect to and interact with chromatindb programmatically, and (3) bump `db/version.h` from `0.1.0` to `0.4.0` with full test verification.

The existing root README.md (218 lines) is well-structured and comprehensive -- it already covers crypto stack, architecture, build/test, usage, configuration, deployment scenarios, and benchmarks. The relocation is primarily a `git mv` followed by augmentation with v0.4.0 features (storage limits, rate limiting, namespace filtering, graceful shutdown, metrics, SIGUSR1/SIGHUP signals) and the new config fields that shipped in Phases 16-18. The root README becomes a minimal 2-3 paragraph pointer.

The protocol walkthrough is a new document. All the information needed to write it exists in the codebase headers: handshake state machines in `db/net/handshake.h`, transport framing in `db/net/framing.h`, wire message types in `schemas/transport.fbs`, blob encoding in `schemas/blob.fbs` and `db/wire/codec.h`, and sync protocol in `db/sync/sync_protocol.h`.

**Primary recommendation:** Treat Plan 19-01 as the bulk of the work (README relocation + augmentation + protocol walkthrough). Plan 19-02 is a trivial version bump (single file edit + test run). No external dependencies, no library research needed -- this phase is purely codebase-introspective documentation.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- Root README becomes a minimal pointer: 2-3 paragraph intro + "See db/README.md for full documentation"
- ALL current root README content moves to db/README.md (crypto stack, architecture, build/test, usage, config, deployment scenarios, benchmarks -- everything)
- db/README.md is the comprehensive operator reference
- Interaction samples are a protocol walkthrough document, NOT runnable code or a C++ integration example
- Walkthrough covers at minimum: connect, PQ handshake, store a signed blob, retrieve it
- Walkthrough lives as a separate file (not inline in README), linked from db/README.md

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
- Versioning narrative -- whether to keep milestone-era section naming or do a full feature refresh
- Whether to add a CHANGELOG.md or rely on README + git history
- License section placement (db/ README, root only, or both)

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| DOC-01 | README.md moved from repo root to db/ (chromatindb is the product, not the repo) | Root README.md (218 lines) exists and is comprehensive. Use `git mv` to preserve history. Root README becomes minimal pointer. |
| DOC-02 | README documents config schema, startup, wire protocol overview, and deployment scenarios | Existing README covers startup, config (7 fields), and 3 scenarios. Must add 4 new config fields from Phases 16-18 (`max_storage_bytes`, `rate_limit_bytes_per_sec`, `rate_limit_burst`, `sync_namespaces`). Must add wire protocol overview section. Must document SIGHUP, SIGUSR1, SIGTERM signal handling. |
| DOC-03 | Interaction samples file showing how to connect to and use the database programmatically | Protocol walkthrough as separate file. Source material: `db/net/handshake.h` (4-step handshake), `db/net/framing.h` (AEAD framing), `schemas/transport.fbs` (24 message types), `db/wire/codec.h` (blob encoding), `db/sync/sync_protocol.h` (sync wire formats). |
| DOC-04 | version.h updated to 0.4.0 after all features pass tests | `db/version.h` currently at `0.1.0`. Three `#define` lines + `VERSION` constexpr to update. Run `ctest --output-on-failure` to verify all 255+ tests pass. |
</phase_requirements>

## Standard Stack

This phase has no library dependencies. It produces documentation files and a single header edit.

### Core
| Asset | Location | Purpose | Current State |
|-------|----------|---------|---------------|
| Root README.md | `/README.md` | Current comprehensive docs (218 lines) | Source material for db/README.md |
| version.h | `/db/version.h` | Version constants (MAJOR/MINOR/PATCH + VERSION string) | `0.1.0`, needs bump to `0.4.0` |
| Config struct | `/db/config/config.h` | Source of truth for all config fields + defaults | 12 fields, all documented in header comments |
| Transport schema | `/schemas/transport.fbs` | Wire message types (24 types) | Complete, source for protocol docs |
| Blob schema | `/schemas/blob.fbs` | Blob wire format (6 fields) | Complete, source for protocol docs |
| Handshake | `/db/net/handshake.h` | PQ handshake state machines (initiator + responder) | Complete, 4-step protocol |
| Framing | `/db/net/framing.h` | AEAD frame format (length-prefixed, counter-nonce) | Complete, well-documented |
| Sync protocol | `/db/sync/sync_protocol.h` | Sync wire format encoders/decoders | Complete, all formats documented |
| Codec | `/db/wire/codec.h` | Blob encode/decode + canonical signing input | Complete, signing input documented |
| PeerManager | `/db/peer/peer_manager.h` | PEX, pub/sub, rate limiting constants | Complete, all constants public |
| Main | `/db/main.cpp` | CLI subcommands (run, keygen, version) | Complete, 3 subcommands |

## Architecture Patterns

### Recommended Documentation Structure

**db/README.md** (comprehensive operator reference):
```
# chromatindb
[intro paragraph]

## Crypto Stack
[existing table -- no changes needed]

## Architecture
[existing content + expand with v0.4.0 features]

## Building
[existing content -- no changes needed]

## Usage
[existing CLI docs -- no changes needed]

## Configuration
[expand with 4 new fields from v0.4.0]

## Signals
[NEW: SIGTERM, SIGHUP, SIGUSR1 documentation]

## Wire Protocol
[NEW: high-level overview of transport framing + message types]

## Scenarios
[existing 3 scenarios + potentially rate-limited/namespace-filtered scenarios]

## Features
[refresh "v3.0 Features" as unified feature list for 0.4.0]

## Performance
[existing benchmarks -- no changes needed]

## License
MIT
```

**db/PROTOCOL.md** (protocol walkthrough -- recommended filename):
```
# chromatindb Protocol Walkthrough

## Transport Layer
[AEAD frame format: 4-byte BE length prefix + ciphertext]

## Connection Lifecycle
### Step 1: TCP Connect
### Step 2: PQ Handshake (ML-KEM-1024 + ML-DSA-87)
### Step 3: Encrypted Session

## Storing a Blob
### Canonical Signing Input
### Data Message

## Retrieving Blobs
### Sync Protocol (Phase A/B/C)

## Additional Interactions
### Blob Deletion (Tombstones)
### Namespace Delegation
### Pub/Sub Notifications
### Peer Exchange
```

### Discretion Recommendations

**Walkthrough format:** Hybrid -- step-by-step narrative with inline diagrams for the handshake. The handshake has 4 clear message exchanges that map naturally to a sequence-style layout using ASCII art (no external diagram tools needed). The rest flows better as narrative with code-style data format descriptions.

**Filename:** `db/PROTOCOL.md` -- concise, standard, discoverable. "Interaction samples" as a phrase is less standard than "protocol walkthrough."

**Wire protocol overview depth:** Message-level detail in the PROTOCOL.md walkthrough, high-level overview in the README (just mention FlatBuffers TransportMessage envelope with type enum + payload, and the AEAD framing). The README section should point to PROTOCOL.md for the full walkthrough.

**FlatBuffers schema references:** Reference them. The schemas are in `schemas/` and are human-readable (blob.fbs is 12 lines, transport.fbs is 38 lines). Including the blob schema inline in PROTOCOL.md makes the data format concrete. Reference transport.fbs for the message type enum.

**Transport framing:** Include it in PROTOCOL.md. The framing is simple (4-byte BE length + AEAD ciphertext) and essential for anyone implementing a client. Omitting it makes the protocol docs incomplete.

**Additional interactions beyond core flow:** Include sync, pub/sub, delegation, and deletion. All are shipped features. Sync is essential for understanding the system. Pub/sub and delegation are differentiating features. Keep each section concise (the core flow gets depth, additional flows get overview treatment).

**Config fields to document:** All 12 fields in the Config struct. The 4 new v0.4.0 fields (`max_storage_bytes`, `rate_limit_bytes_per_sec`, `rate_limit_burst`, `sync_namespaces`) need explicit documentation with their defaults and behavior.

**Deployment scenarios:** Keep existing three (single node, two-node sync, closed mode with ACLs). Add one new scenario: "Rate-Limited Public Node" showing `max_storage_bytes` + `rate_limit_bytes_per_sec` config for an open node that protects against abuse. This demonstrates the v0.4.0 production readiness features.

**Versioning narrative:** Full feature refresh. Replace "v3.0 Features" with a unified feature list under the new version. The milestone-era naming (v1.0/v2.0/v3.0) is internal development history, not user-facing. The operator sees one product at version 0.4.0.

**CHANGELOG.md:** Skip. The project has no release history yet (version has been 0.1.0 throughout development). A changelog becomes useful when there are actual releases to track. For now, README + git history is sufficient. YAGNI.

**License placement:** Both. MIT in root (standard location for GitHub/hosting) and a one-liner in db/README.md pointing to root LICENSE or just saying "MIT". Since there's no LICENSE file currently, add the license line in db/README.md matching the existing root README pattern.

**Confidence:** HIGH -- all recommendations are based on direct inspection of the existing codebase and standard documentation practices.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Sequence diagrams | External diagram generation tools or complex Mermaid | ASCII art in markdown | Zero dependencies, renders everywhere, handshake is only 4 messages |
| Config field documentation | Manual enumeration | Read directly from `db/config/config.h` Config struct | Source of truth with defaults and doc comments |
| Wire format descriptions | Free-form prose | Reference the FlatBuffers `.fbs` schemas (12-38 lines each) | Schemas ARE the documentation |

## Common Pitfalls

### Pitfall 1: Stale Documentation After Move
**What goes wrong:** Content is moved to db/README.md but root README still contains old content or links break.
**Why it happens:** Git mv preserves history but doesn't update cross-references.
**How to avoid:** After moving, verify root README is replaced entirely with the pointer. Check for any internal links in the moved content that reference root paths.
**Warning signs:** Root README still has config examples or usage docs.

### Pitfall 2: Undocumented Config Fields
**What goes wrong:** New v0.4.0 config fields (max_storage_bytes, rate_limit_bytes_per_sec, rate_limit_burst, sync_namespaces) are omitted from the config schema section.
**Why it happens:** The existing README was written for v3.0 and only has 7 of the 12 Config struct fields.
**How to avoid:** Cross-reference the Config struct in `db/config/config.h` line by line against the README config section. Every field in the struct must appear in the docs.
**Warning signs:** Config section has 7 fields but Config struct has 12 (minus `config_path` which is internal, `storage_path` which is derived -- so 10 user-facing fields).

### Pitfall 3: Version Bump Before Tests Pass
**What goes wrong:** Version is bumped to 0.4.0 but a test fails, leaving the codebase in a broken state.
**Why it happens:** Plan 19-02 bumps version before running tests.
**How to avoid:** Run the full test suite BEFORE the version bump to confirm baseline is green, then bump, then run again to confirm the bump didn't break anything. DOC-04 requirement explicitly says "after all features pass tests."
**Warning signs:** `ctest` output shows failures.

### Pitfall 4: Missing Signal Documentation
**What goes wrong:** The README documents config and CLI but omits runtime signal handling (SIGTERM, SIGHUP, SIGUSR1) that shipped in Phases 17 and 18.
**Why it happens:** Signals are not in the original README because they were added later.
**How to avoid:** Add a dedicated "Signals" section documenting SIGTERM (graceful shutdown), SIGHUP (config reload -- re-reads allowed_keys), and SIGUSR1 (metrics dump to log).
**Warning signs:** No mention of signals in the README.

### Pitfall 5: Protocol Walkthrough Too Abstract or Too Detailed
**What goes wrong:** The walkthrough is either so high-level it's useless for implementation, or so detailed it duplicates the source code.
**Why it happens:** No clear target audience definition.
**How to avoid:** Write for "developer building a client in another language." They need: byte-level transport framing, message type enum values, handshake message sequence, blob data format, and canonical signing input construction. They do NOT need: C++ API specifics, internal state machine details, or test infrastructure.
**Warning signs:** Protocol doc references C++ types instead of byte layouts.

## Code Examples

### version.h Current State (verified)
```c
// db/version.h -- current
#define VERSION_MAJOR "0"
#define VERSION_MINOR "1"
#define VERSION_PATCH "0"

static constexpr const char* VERSION = VERSION_MAJOR "." VERSION_MINOR "." VERSION_PATCH;
```

### version.h Target State
```c
// db/version.h -- target
#define VERSION_MAJOR "0"
#define VERSION_MINOR "4"
#define VERSION_PATCH "0"

static constexpr const char* VERSION = VERSION_MAJOR "." VERSION_MINOR "." VERSION_PATCH;
```

### Config Struct (all fields, source of truth)
```cpp
// db/config/config.h -- Config struct (12 fields total)
struct Config {
    std::string bind_address = "0.0.0.0:4200";
    std::string storage_path = "./data/blobs";       // internal, derived from data_dir
    std::string data_dir = "./data";
    std::vector<std::string> bootstrap_peers;
    std::string log_level = "info";
    uint32_t max_peers = 32;
    uint32_t sync_interval_seconds = 60;
    uint64_t max_storage_bytes = 0;                   // v0.4.0: 0 = unlimited
    uint64_t rate_limit_bytes_per_sec = 0;            // v0.4.0: 0 = disabled
    uint64_t rate_limit_burst = 0;                    // v0.4.0: 0 = disabled
    std::vector<std::string> sync_namespaces;         // v0.4.0: empty = all
    std::vector<std::string> allowed_keys;
    std::filesystem::path config_path;                // internal, not user-facing
};
```

User-facing config fields to document (10 total):
1. `bind_address` (existing)
2. `data_dir` (existing)
3. `bootstrap_peers` (existing)
4. `allowed_keys` (existing)
5. `max_peers` (existing)
6. `sync_interval_seconds` (existing)
7. `log_level` (existing)
8. `max_storage_bytes` (NEW -- v0.4.0)
9. `rate_limit_bytes_per_sec` (NEW -- v0.4.0)
10. `rate_limit_burst` (NEW -- v0.4.0)
11. `sync_namespaces` (NEW -- v0.4.0)

Note: `storage_path` is derived from `data_dir` internally and `config_path` is set by CLI parsing. Neither is user-configurable via JSON.

### Transport Framing (for protocol walkthrough)
```
Frame format (after handshake, encrypted session):
  [4 bytes: big-endian uint32 ciphertext_length]
  [ciphertext_length bytes: AEAD ciphertext]

AEAD parameters:
  Algorithm: ChaCha20-Poly1305 (IETF)
  Key: 32 bytes (derived from ML-KEM shared secret via HKDF)
  Nonce: 12 bytes (4 zero bytes + 8-byte big-endian counter)
  AD: empty
  Tag: 16 bytes (appended to ciphertext)

Plaintext is a FlatBuffers TransportMessage:
  type: TransportMsgType (byte enum)
  payload: [ubyte] (variable, type-dependent)

Counter: separate send/recv counters, each starting at 0, incrementing per frame.
Max frame size: 110 MiB (115,343,360 bytes).
```

### Handshake Sequence (for protocol walkthrough)
```
Initiator                         Responder
    |                                 |
    |--- [raw] KemPubkey ----------->|  ML-KEM-1024 ephemeral pubkey (1568 B)
    |                                 |  Responder encapsulates: (ciphertext, shared_secret)
    |<-- [raw] KemCiphertext --------|  ML-KEM-1024 ciphertext (1568 B)
    |                                 |
    |   Both derive session keys via HKDF:
    |   - "chromatin-init-to-resp-v1" (initiator->responder key)
    |   - "chromatin-resp-to-init-v1" (responder->initiator key)
    |   - session_fingerprint for mutual auth
    |                                 |
    |--- [encrypted] AuthSignature ->|  ML-DSA-87 pubkey (2592 B) + signature
    |<-- [encrypted] AuthSignature --|  ML-DSA-87 pubkey (2592 B) + signature
    |                                 |
    |   Session established. All subsequent messages are AEAD-encrypted.
    |   ACL check happens here (if allowed_keys configured).
```

### Sync Protocol Flow (for protocol walkthrough)
```
Phase A: Namespace exchange
  Initiator -> SyncRequest (empty payload)
  Responder -> SyncAccept (empty payload)
  Both exchange NamespaceList: [count:u32BE][ns:32B + seq:u64BE]...

Phase B: Per-namespace hash diff (one namespace at a time)
  Both exchange HashList: [ns:32B][count:u32BE][hash:32B]...
  Each side computes diff (hashes peer has that we don't)

Phase C: Blob transfer (one blob at a time)
  Requester -> BlobRequest: [ns:32B][count:u32BE][hash:32B]...  (max 64 hashes)
  Responder -> BlobTransfer: [count:u32BE][len:u32BE][flatbuf blob]...
  Requester validates + ingests each blob

Both -> SyncComplete (empty payload)
PEX follows inline after sync completes.
```

## State of the Art

Not applicable -- this phase produces documentation, not code with library dependencies.

## Open Questions

None. All information needed is available in the codebase headers and the existing README. The discretion areas have clear recommendations based on standard practices and the specific codebase structure.

## Sources

### Primary (HIGH confidence)
- `/README.md` -- existing 218-line comprehensive README (inspected directly)
- `/db/version.h` -- current version `0.1.0` (inspected directly)
- `/db/config/config.h` -- Config struct with all 12 fields and defaults (inspected directly)
- `/db/net/handshake.h` -- 4-step handshake protocol with initiator/responder state machines (inspected directly)
- `/db/net/framing.h` -- AEAD frame format, MAX_FRAME_SIZE, nonce construction (inspected directly)
- `/db/net/connection.h` -- Connection lifecycle, send/recv counters (inspected directly)
- `/db/net/protocol.h` -- TransportCodec encode/decode (inspected directly)
- `/schemas/transport.fbs` -- 24 TransportMsgType values (inspected directly)
- `/schemas/blob.fbs` -- Blob table with 6 fields (inspected directly)
- `/db/wire/codec.h` -- BlobData struct, canonical signing input, tombstone/delegation utilities (inspected directly)
- `/db/sync/sync_protocol.h` -- Sync wire format encoders with documented wire layouts (inspected directly)
- `/db/peer/peer_manager.h` -- PeerInfo, NodeMetrics, protocol constants (inspected directly)
- `/db/engine/engine.h` -- IngestError enum, BlobEngine API (inspected directly)
- `/db/storage/storage.h` -- 5 sub-databases, Storage API (inspected directly)
- `/db/main.cpp` -- CLI subcommands: run, keygen, version (inspected directly)
- `/CMakeLists.txt` -- Full dependency list with versions (inspected directly)
- `/.planning/REQUIREMENTS.md` -- DOC-01 through DOC-04 requirements (inspected directly)
- `/.planning/STATE.md` -- Phase 18 complete, all decisions logged (inspected directly)
- `/.planning/PROJECT.md` -- Key decisions table, constraints, scope (inspected directly)

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- no external libraries needed, all source material is in the codebase
- Architecture: HIGH -- documentation structure follows standard patterns and is constrained by user decisions
- Pitfalls: HIGH -- identified from direct inspection of existing README vs codebase state

**Research date:** 2026-03-12
**Valid until:** 2026-04-12 (stable -- documentation of existing code)
