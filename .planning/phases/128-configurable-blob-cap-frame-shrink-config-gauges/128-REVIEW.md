---
phase: 128-configurable-blob-cap-frame-shrink-config-gauges
reviewed: 2026-04-23T03:05:28Z
depth: standard
files_reviewed: 21
files_reviewed_list:
  - db/config/config.h
  - db/config/config.cpp
  - db/net/framing.h
  - db/net/connection.h
  - db/net/connection.cpp
  - db/engine/engine.h
  - db/engine/engine.cpp
  - db/peer/message_dispatcher.h
  - db/peer/message_dispatcher.cpp
  - db/peer/peer_manager.h
  - db/peer/peer_manager.cpp
  - db/peer/metrics_collector.h
  - db/peer/metrics_collector.cpp
  - db/main.cpp
  - cli/src/connection.cpp
  - cli/src/commands.cpp
  - db/tests/config/test_config.cpp
  - db/tests/peer/test_metrics_endpoint.cpp
  - db/tests/engine/test_engine.cpp
  - db/tests/net/test_framing.cpp
  - db/tests/peer/test_peer_manager.cpp
findings:
  critical: 0
  warning: 2
  info: 4
  total: 6
status: issues_found
---

# Phase 128: Code Review Report

**Reviewed:** 2026-04-23T03:05:28Z
**Depth:** standard
**Files Reviewed:** 21
**Status:** issues_found

## Summary

Phase 128 delivers four interlocking changes (FRAME_SIZE shrink, HARD_CEILING rename,
operator-tunable `Config::blob_max_bytes`, 24 config gauges) plus a load-bearing
refactor of `PeerManager::config_` from `const Config&` to an owned value. The
core mechanisms are correct:

- **Seeded-member pattern (BLOB-01/03/04):** All four reader sites
  (`BlobEngine::ingest`, `Connection::recv_chunked`, `MessageDispatcher`
  NodeInfoResponse, PeerManager ctor+reload) read their seeded `blob_max_bytes_`
  u64 once per boundary check. All reads and writes happen on the single
  `ioc.run()` thread (confirmed in `db/main.cpp:473`). `reload_config()` is
  invoked synchronously from `handle_sighup()` on the io_context thread
  (peer_manager.cpp:493-512), so no coroutine can interleave a partial-write.
  No torn-read hazard, no data race.

- **Owned `config_` lifetime (METRICS-02):** `MetricsCollector` holds
  `const config::Config& config_` pointing at PeerManager's owned
  member `config_`. PeerManager declaration order puts `config_` before
  `metrics_collector_` (peer_manager.h:150 vs :174), so the reference is
  valid at construction. The final `config_ = new_cfg;` at
  peer_manager.cpp:751 assigns into the SAME member object; the reference
  stays valid. Correct.

- **24 config gauges:** Alphabetically ordered, numeric fields only;
  enumeration matches the numeric-field set of `struct Config` exactly
  (24 of 24, verified by `grep -Eo "config_\.[a-z_]+" metrics_collector.cpp`).
  D-07 exclusion list (14 non-numeric fields) is asserted absent in
  `test_metrics_endpoint.cpp:203-216`.

- **FRAME shrink 110 MiB → 2 MiB:** The static_asserts in
  `db/net/framing.h:38-46` enforce the invariant `2×STREAMING_THRESHOLD ≤
  MAX_FRAME_SIZE ≤ 2×STREAMING_THRESHOLD + TRANSPORT_ENVELOPE_MARGIN`,
  pinning the change to compile time. The client mirror in
  `cli/src/connection.cpp:36-42` duplicates the lower-bound assert.
  `recv_chunked` (connection.cpp:855) now enforces `total_payload_size >
  blob_max_bytes_` (64 MiB ceiling) NOT `MAX_FRAME_SIZE` (2 MiB), so
  a legitimate 64 MiB chunked stream is not rejected by the shrunk per-frame
  limit. Correct.

Two Warnings are issue-worthy: an unscanned stale `const Config&` reference
in `net::Server` that survives the PeerManager refactor, and a D-17 error
message that shows the live value but does not name the `blob_max_bytes`
tunable operators need to tune. Four Info items cover stale documentation
(README/PROTOCOL/ARCHITECTURE) not in the scoped file list but discovered
during cross-reference.

## Warnings

### WR-01: `net::Server::config_` retains `const Config&` to caller's Config, not PeerManager's owned member

**File:** `db/net/server.h:137`
**Issue:** The PeerManager ctor refactor (peer_manager.cpp:43 `server_(config, ...)`)
passes the CONSTRUCTOR PARAMETER `const config::Config& config` to `net::Server`.
Server stores this as `const config::Config& config_` (server.h:137). Meanwhile
PeerManager also copies that same parameter into its owned member `config_` at
peer_manager.cpp:36. On SIGHUP, `reload_config()` mutates ONLY the owned
`PeerManager::config_` (line 751); `Server::config_` still references the
CALLER's Config object (the `cfg` local in `cmd_run`, `db/main.cpp:379`). Post-SIGHUP,
`Server::config_` and `PeerManager::config_` diverge.

Currently benign because Server only reads `config_.bind_address` (start-only)
and `config_.bootstrap_peers` (start-only, see server.cpp:30,47); neither is
SIGHUP-reloadable. But this is a latent trap: any future code that reads a
SIGHUP-reloadable field through `Server::config_` will silently observe stale
values. The refactor comment at peer_manager.cpp:747-751 claims "this single
assignment updates all live readers atomically", which is true for
MetricsCollector (which references the owned member) but NOT for Server.

**Fix:** Either (a) change `server_(config, ...)` to `server_(config_, ...)`
in the PeerManager initializer list so Server's reference tracks the owned
member — but note member init order: `server_` is declared on line 158 of
peer_manager.h, AFTER `config_` on line 150, so `config_` is initialized
first; this change is safe. Or (b) drop `net::Server`'s `Config&` member
entirely and pass the two fields it actually needs (`bind_address`,
`bootstrap_peers`) by value at construction. Option (a) is the minimal
change:

```cpp
// db/peer/peer_manager.cpp:43 (change `config` to `config_`)
    : config_(config)
    , identity_(identity)
    , engine_(engine)
    , storage_(storage)
    , ioc_(ioc)
    , pool_(pool)
    , acl_(acl)
    , server_(config_, identity, ioc)  // was: server_(config, identity, ioc)
```

### WR-02: IngestError::oversized_blob detail string does not name the `blob_max_bytes` tunable

**File:** `db/engine/engine.cpp:112-118`
**Issue:** The rejection detail reads `"blob data size N exceeds max M"` where
`N` is `blob.data.size()` and `M` is `blob_max_bytes_`. This satisfies D-17's
"shows current value" requirement (and the `test_config.cpp:1830-1831` test
that checks for `"2097152"` substring passes), but it does not name the
operator-facing config field. An operator reading the node log or an error
response has no cue that the knob to turn is `blob_max_bytes` in the config
file. The review brief explicitly calls out "names the tunable, shows current
value" as the D-17 bar.

**Fix:** Name the field in both the spdlog line and the rejection detail:

```cpp
// db/engine/engine.cpp:112-118
if (blob.data.size() > blob_max_bytes_) {
    spdlog::warn("Ingest rejected: blob data size {} exceeds blob_max_bytes={}",
                 blob.data.size(), blob_max_bytes_);
    co_return IngestResult::rejection(IngestError::oversized_blob,
        "blob data size " + std::to_string(blob.data.size()) +
        " exceeds blob_max_bytes=" + std::to_string(blob_max_bytes_));
}
```

The existing `test_config.cpp:1830-1831` substring check `"2097152"` still
passes. Tests in `test_engine.cpp:553` that grep for the size value also
continue to pass.

## Info

### IN-01: Stale `MAX_BLOB_DATA_SIZE` / 110 MiB references in docs

**File:** `db/ARCHITECTURE.md:361`, `db/PROTOCOL.md:25`, `db/README.md:230`
**Issue:** Three documentation files still reference the pre-Phase-128
constants:
- `ARCHITECTURE.md:361` says `blob.data.size() <= MAX_BLOB_DATA_SIZE` — the
  symbol was renamed to `MAX_BLOB_DATA_HARD_CEILING` and the runtime check is
  now against `Config::blob_max_bytes`, not the build-time ceiling.
- `PROTOCOL.md:25` and `README.md:230` both claim "maximum frame size is 110
  MiB" — the actual post-FRAME-01 value is 2 MiB.

Out of the scoped review files (these are not in `config.files`), but they're
user-facing protocol documentation and will mislead anyone reading the repo.

**Fix:** Update the three lines:
- `ARCHITECTURE.md:361`: `MAX_BLOB_DATA_SIZE` → `Config::blob_max_bytes (live cap)`; update engine.cpp line cite from `:109` to `:112`.
- `PROTOCOL.md:25`: `110 MiB (115,343,360 bytes)` → `2 MiB (2,097,152 bytes)`.
- `README.md:230`: `110 MiB` → `2 MiB`.

### IN-02: Stale 110 MiB guard in `test_connection.cpp` helper (out of scope, but adjacent)

**File:** `db/tests/net/test_connection.cpp:795`
**Issue:** An inline `recv_raw` lambda in the test harness checks `if (len >
110 * 1024 * 1024) co_return std::nullopt;`. This file is not in the Phase
128 scoped review list, but it's adjacent to the changes and the literal is
now stale against the real `MAX_FRAME_SIZE=2 MiB`. Test fidelity: the harness
is more permissive than the production code under test.

**Fix:** Replace the literal with `chromatindb::net::MAX_FRAME_SIZE`:

```cpp
// db/tests/net/test_connection.cpp:795
if (len > chromatindb::net::MAX_FRAME_SIZE) co_return std::nullopt;
```

### IN-03: `MAX_FRAME_SIZE` is duplicated in `cli/src/connection.cpp` without cross-repo `static_assert`

**File:** `cli/src/connection.cpp:36-44`
**Issue:** `cli/src/connection.cpp` defines its own `MAX_FRAME_SIZE = 2 *
1024 * 1024` as a file-scope `constexpr`, duplicating `db/net/framing.h:16`.
The two repos are separate binaries but MUST agree on wire framing — any
future drift (db says 4 MiB, cli still says 2 MiB, or vice-versa) is a
silent handshake break. The comment at cli/src/connection.cpp:36 notes
"2 MiB (Phase 128 FRAME-01)" but there's no compile-time link between the
two values.

YAGNI caveat: cdb and the node are in the same repo so a cross-directory
`static_assert` is mechanically possible via a shared header. But
`cli/src/connection.cpp` deliberately avoids `db/` headers to keep cli
self-contained. Accepting the duplication matches that design.

**Fix:** Optional — add a paired comment cross-reference at both sites
pinning the invariant:

```cpp
// cli/src/connection.cpp:36
// MUST equal db/net/framing.h MAX_FRAME_SIZE. Any change must be made in
// both places simultaneously — drift silently breaks the wire protocol.
static constexpr uint32_t MAX_FRAME_SIZE = 2 * 1024 * 1024;  // 2 MiB (Phase 128 FRAME-01)
```

No code change required; this is a documentation-only tightening.

### IN-04: CLI `MAX_FILE_SIZE = 500 MiB` in non-chunked put path exceeds node's 64 MiB hard ceiling

**File:** `cli/src/commands.cpp:674`
**Issue:** The non-chunked `put` path caps per-file uploads at 500 MiB:
```cpp
static constexpr size_t MAX_FILE_SIZE = 500ULL * 1024 * 1024;
```
Any file in `[64 MiB+1, 400 MiB)` that skips the chunked path
(`CHUNK_THRESHOLD_BYTES=400 MiB`) will be sent as a single blob and rejected
by the node (which now enforces `blob_max_bytes` default 4 MiB, hard ceiling
64 MiB). The comment at commands.cpp:672-673 acknowledges "Node enforces
Config::blob_max_bytes (default 4 MiB, advertised via NodeInfoResponse)" but
the client-side cap is not aligned.

This is pre-existing to Phase 128 and can be handled more carefully in a
follow-up by (a) reading the advertised cap from NodeInfoResponse (the
node already exposes it per NODEINFO-01 at message_dispatcher.cpp:721) and
(b) rejecting upfront with a clear message. Not a Phase 128 regression.

**Fix:** Tracked for a future phase; no change required here. The current
behaviour is "node rejects with a clear oversized_blob error", which is
correct albeit wasteful of one round-trip.

---

_Reviewed: 2026-04-23T03:05:28Z_
_Reviewer: Claude (gsd-code-reviewer)_
_Depth: standard_
