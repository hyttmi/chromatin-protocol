---
phase: 128-configurable-blob-cap-frame-shrink-config-gauges
verified: 2026-04-23T14:00:00Z
status: passed
score: 5/5 roadmap success criteria verified
overrides_applied: 0
re_verification: false
---

# Phase 128: Configurable Blob Cap + Frame Shrink + Config Gauges — Verification Report

**Phase Goal:** Operators can set the blob cap in `config.json`, hot-reload it with SIGHUP, and verify it remotely via `/metrics` — while the frame size drops to a 2 MiB protocol invariant that reflects the actual streaming reality.
**Verified:** 2026-04-23T14:00:00Z
**Status:** PASSED
**Re-verification:** No — initial verification

## Goal Achievement

### Observable Truths (Roadmap Success Criteria)

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| 1 | `blob_max_bytes` in `config.json` (default 4 MiB, bounds [1 MiB, 64 MiB]) controls the in-memory blob cap; out-of-range values fail config load with a clear error message | VERIFIED | `Config::blob_max_bytes = 4ULL * 1024 * 1024` in config.h:26; `validate_config` bounds check in config.cpp:267-276 using accumulator pattern; `chromatindb::net::MAX_BLOB_DATA_HARD_CEILING` symbol used for upper bound (no hardcoded literal); errors name the field and received value |
| 2 | SIGHUP reloads `blob_max_bytes`: new writes honor the new cap, existing stored blobs remain readable when cap is lowered (no migration, no deletion), oversized-ingest rejection uses the current cap in its error message | VERIFIED | Engine reads `blob_max_bytes_` seeded member (engine.cpp:112); PeerManager seeds engine+dispatcher at construct (peer_manager.cpp:172-173) and SIGHUP reload (peer_manager.cpp:582-589); live connections seeded at TCP/UDS on_connected chokepoints (peer_manager.cpp:213, 229) and iterated on SIGHUP (peer_manager.cpp:584-587); D-17: rejection message reads `std::to_string(blob_max_bytes_)` at engine.cpp:117; `config_ = new_cfg` at peer_manager.cpp:751 makes reload live |
| 3 | `MAX_FRAME_SIZE` is 2 MiB, pinned by a `static_assert` documenting `MAX_FRAME_SIZE ≈ 2 × STREAMING_THRESHOLD` so a future tweak fails the build | VERIFIED | `constexpr uint32_t MAX_FRAME_SIZE = 2 * 1024 * 1024` at framing.h:16; CLI mirrors at cli/src/connection.cpp:36; paired lower-bound assert at framing.h:38 (`>= 2 * STREAMING_THRESHOLD`) and upper-bound assert at framing.h:44 (`<= 2 * STREAMING_THRESHOLD + TRANSPORT_ENVELOPE_MARGIN`); `TRANSPORT_ENVELOPE_MARGIN = 64` at framing.h:36 |
| 4 | A `/metrics` scrape returns a `chromatindb_config_*` gauge for every numeric `Config` field (string fields excluded); after SIGHUP the next scrape returns updated values without node restart | VERIFIED | 24 gauges emitted alphabetically in metrics_collector.cpp:359-451, each via `std::to_string(config_.X)` reading the live `const config::Config&` bound to PeerManager's owned `config_` member; `PeerManager::config_` changed from `const config::Config&` to `config::Config` (owned value) at peer_manager.h:150; `config_ = new_cfg` in reload_config ensures post-SIGHUP reads return new values; zero excluded string/vector/map fields found in emission block |
| 5 | Unit tests cover config load, bounds validation, SIGHUP reload, and `chromatindb_config_*` gauge emission | VERIFIED | 6 `[config][blob-cap]` TEST_CASEs in test_config.cpp (default, JSON parse, lower-bound reject, upper-bound reject, accept-at-boundaries, BlobEngine SIGHUP reload via `set_blob_max_bytes`); 3 `[metrics][prometheus][config-gauge]` TEST_CASEs in test_metrics_endpoint.cpp (existence + exclusions 38 matchers, construction-time value fidelity, reload-scrape fidelity via `pm.reload_config()`); targeted tag runs passed per 128-05-SUMMARY: `[config][blob-cap]` 18 assertions/6 cases, `[metrics][prometheus][config-gauge]` 43 assertions/3 cases, `[sighup]` 9 assertions/2 cases |

**Score:** 5/5 truths verified

### Deferred Items

None. All Phase 128 roadmap criteria fully met in-phase. Items belonging to Phase 129 (sync-out cap filtering) are explicitly absent per D-16 and confirmed by grep — no `blob_max_bytes` enforcement found on read path, sync-out path, GC path, or compaction path.

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `db/net/framing.h` | MAX_FRAME_SIZE=2 MiB; MAX_BLOB_DATA_HARD_CEILING=64 MiB; TRANSPORT_ENVELOPE_MARGIN; paired static_asserts | VERIFIED | Line 16: 2 MiB; line 25: 64 MiB; line 36: margin=64; lines 38+44: paired asserts; zero `MAX_BLOB_DATA_SIZE` occurrences |
| `cli/src/connection.cpp` | CLI MAX_FRAME_SIZE mirror at 2 MiB | VERIFIED | Line 36: `static constexpr uint32_t MAX_FRAME_SIZE = 2 * 1024 * 1024;`; no 110 MiB literal or comment remains |
| `db/config/config.h` | Config::blob_max_bytes u64 with 4 MiB default | VERIFIED | Line 26: `uint64_t blob_max_bytes = 4ULL * 1024 * 1024;` with BLOB-01/02 anchor |
| `db/config/config.cpp` | blob_max_bytes parsed + in known_keys + bounds validated | VERIFIED | Parse at line 42; known_keys at line 74; bounds check at lines 267-276 using `MAX_BLOB_DATA_HARD_CEILING` symbol; `#include "db/net/framing.h"` at line 2 |
| `db/engine/engine.h` | BlobEngine::set_blob_max_bytes setter + blob_max_bytes_ member | VERIFIED | Setter at line 108 (inline); member at line 196 with 4 MiB default |
| `db/peer/message_dispatcher.h` | MessageDispatcher::set_blob_max_bytes setter + blob_max_bytes_ member | VERIFIED | Setter at line 62; member at line 88 |
| `db/net/connection.h` | Connection::set_blob_max_bytes setter + blob_max_bytes_ member | VERIFIED | Setter at line 127; member at line 240 |
| `db/peer/peer_manager.cpp` | Three seeding/reload call pairs for engine/dispatcher/live-connections | VERIFIED | Constructor: lines 172-173; per-connection TCP+UDS: lines 213, 229; SIGHUP reload: lines 582-587; log at line 589 |
| `db/peer/peer_manager.h` | config_ as owned value (not const-ref) | VERIFIED | Line 150: `config::Config config_;` |
| `db/peer/metrics_collector.cpp` | 24 chromatindb_config_* gauges alphabetical | VERIFIED | 24 TYPE annotations counted; emission order confirmed alphabetical; `const config::Config& config_` member reads live PeerManager::config_ |
| `db/tests/config/test_config.cpp` | 6 [config][blob-cap] TEST_CASEs | VERIFIED | 6 TEST_CASEs confirmed; includes for framing.h, engine.h, storage.h, test_helpers.h, asio.hpp present |
| `db/tests/peer/test_metrics_endpoint.cpp` | 3 [metrics][prometheus][config-gauge] TEST_CASEs | VERIFIED | 3 TEST_CASEs confirmed; 43 ContainsSubstring matchers total; `pm.reload_config()` called in [sighup] test |
| `db/tests/engine/test_engine.cpp` | MAX_BLOB_DATA_SIZE migrated to MAX_BLOB_DATA_HARD_CEILING | VERIFIED | Zero `MAX_BLOB_DATA_SIZE` refs; `set_blob_max_bytes(MAX_BLOB_DATA_HARD_CEILING)` at line 482 |
| `db/tests/net/test_framing.cpp` | Old constants asserted at new values | VERIFIED | `MAX_BLOB_DATA_HARD_CEILING == 64ULL * 1024 * 1024` at line 191; `MAX_FRAME_SIZE == 2u * 1024 * 1024` at line 198; zero old 110 MiB or 500 MiB literals |
| `db/tests/peer/test_peer_manager.cpp` | NodeInfoResponse assertion reads cfg.blob_max_bytes | VERIFIED | Line 2900: `CHECK(max_blob_data_bytes == cfg.blob_max_bytes)` |

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| PeerManager::reload_config | BlobEngine::set_blob_max_bytes + MessageDispatcher::set_blob_max_bytes + per-connection set_blob_max_bytes | config reload triggered on SIGHUP | WIRED | peer_manager.cpp:582-587 has all three calls; pattern `engine_.set_blob_max_bytes(new_cfg.blob_max_bytes)` confirmed |
| BlobEngine::ingest Step 0 | blob_max_bytes_ member | direct member read | WIRED | engine.cpp:112: `blob.data.size() > blob_max_bytes_` |
| Connection::handle_chunked_data | Connection::blob_max_bytes_ member | direct member read | WIRED | connection.cpp:855: `total_payload_size > blob_max_bytes_` |
| MessageDispatcher NodeInfoResponse encoder | MessageDispatcher::blob_max_bytes_ member | direct member read | WIRED | message_dispatcher.cpp:721: `store_u64_be(response.data() + off, blob_max_bytes_)` |
| format_prometheus_metrics emission | config_ member reference (live read each scrape) | direct field read, no cache | WIRED | metrics_collector.cpp:359-451: 24 blocks read `config_.<field>` via `std::to_string`; config_ is a reference to PeerManager's owned value |
| PeerManager::config_ | MetricsCollector::config_ | const Config& reference bound to owned member | WIRED | MetricsCollector constructed with `config_` (member), not `config` (constructor parameter) per 128-04-SUMMARY |
| reload_config | config_ = new_cfg | owned-value mutation after validation | WIRED | peer_manager.cpp:751: `config_ = new_cfg;` after all setter calls |

### Data-Flow Trace (Level 4)

| Artifact | Data Variable | Source | Produces Real Data | Status |
|----------|---------------|--------|--------------------|--------|
| `db/engine/engine.cpp` ingest Step 0 | `blob_max_bytes_` | Seeded from `config.blob_max_bytes` at PeerManager constructor + reseeded on SIGHUP | Yes — real Config field, not hardcoded | FLOWING |
| `db/net/connection.cpp` handle_chunked_data | `blob_max_bytes_` | Seeded via `conn->set_blob_max_bytes(config_.blob_max_bytes)` at TCP+UDS on_connected | Yes — live PeerManager::config_ value | FLOWING |
| `db/peer/message_dispatcher.cpp` NodeInfoResponse | `blob_max_bytes_` | Seeded from `config.blob_max_bytes` at PeerManager constructor + SIGHUP | Yes — real Config field | FLOWING |
| `db/peer/metrics_collector.cpp` format_prometheus_metrics | `config_.<field>` | const Config& bound to PeerManager::config_ (owned value, mutated on SIGHUP) | Yes — live struct reads, not cached | FLOWING |

### Behavioral Spot-Checks

Targeted tag runs were executed inside the executor worktree per 128-05-SUMMARY. Build gates were confirmed by executors. Full test suite run is user-delegated per `feedback_delegate_tests_to_user.md`. The following spot-checks were verified by reading SUMMARY evidence + actual test code:

| Behavior | Evidence | Status |
|----------|----------|--------|
| `[config][blob-cap]` 6 TEST_CASEs build and pass | 128-05-SUMMARY: 18 assertions / 6 cases PASSED; test code confirmed in test_config.cpp | PASS |
| `[metrics][prometheus][config-gauge]` 3 TEST_CASEs build and pass | 128-05-SUMMARY: 43 assertions / 3 cases PASSED; test code confirmed in test_metrics_endpoint.cpp | PASS |
| `[sighup]` 2 TEST_CASEs build and pass | 128-05-SUMMARY: 9 assertions / 2 cases PASSED; test code confirmed (test_config.cpp + test_metrics_endpoint.cpp) | PASS |
| `chromatindb` daemon target builds green | 128-03-SUMMARY: `cmake --build build-debug -j$(nproc) --target chromatindb` exit 0 after Wave 3 | PASS |
| `chromatindb_tests` target builds green | 128-05-SUMMARY: `cmake --build build-debug -j$(nproc) --target chromatindb_tests` exit 0 after Wave 4 | PASS |

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|------------|-------------|--------|----------|
| BLOB-01 | 128-01, 128-03 | Replace hardcoded MAX_BLOB_DATA_SIZE with Config::blob_max_bytes; default 4 MiB | SATISFIED | Field declared in config.h:26; runtime callsites (engine.cpp:112, connection.cpp:855, message_dispatcher.cpp:721) all read seeded blob_max_bytes_ member |
| BLOB-02 | 128-02 | Config bounds validation [1 MiB, 64 MiB] at startup | SATISFIED | validate_config bounds check at config.cpp:267-276; uses MAX_BLOB_DATA_HARD_CEILING symbol; accumulator pattern; field-named error messages |
| BLOB-03 | 128-03 | SIGHUP-reloadable; new writes honor new cap; existing blobs readable | SATISFIED | PeerManager::reload_config seeds engine + dispatcher + every live connection (peer_manager.cpp:582-587); D-14 ingest-only enforcement confirmed — read path, sync-out, GC have no cap check (grep confirms) |
| BLOB-04 | 128-03 | Ingest rejection error message reflects the actual current cap | SATISFIED | engine.cpp:117: `std::to_string(blob_max_bytes_)` in rejection message; test_config.cpp [sighup] test verifies "2097152" appears in error_detail after `set_blob_max_bytes(2 MiB)` |
| FRAME-01 | 128-01 | Lower MAX_FRAME_SIZE from 110 MiB to 2 MiB in both db/ and cli/ | SATISFIED | framing.h:16 and cli/src/connection.cpp:36 both show 2 MiB; atomic single commit (999f5d5b) per D-10 |
| FRAME-02 | 128-01 | MAX_FRAME_SIZE fixed protocol constant; static_assert documents relationship | SATISFIED | Paired asserts at framing.h:38 (lower bound) and framing.h:44 (upper bound); references STREAMING_THRESHOLD and TRANSPORT_ENVELOPE_MARGIN by name |
| METRICS-01 | 128-04 | Every numeric Config field exposed as chromatindb_config_<field> gauge | SATISFIED | 24 gauges emitted alphabetically; all 24 numeric fields covered; string/vector/map fields excluded |
| METRICS-02 | 128-04 | Gauges reflect live values after SIGHUP without node restart | SATISFIED | PeerManager::config_ owned value; reload_config assigns `config_ = new_cfg` (peer_manager.cpp:751); MetricsCollector reads via const Config& reference; reload-scrape test passes |
| VERI-01 | 128-05 | Unit tests for blob_max_bytes config load, bounds validation, SIGHUP reload | SATISFIED | 6 [config][blob-cap] TEST_CASEs + 1 [config][blob-cap][sighup] engine-layer test + 1 [metrics][prometheus][config-gauge][sighup] PeerManager-layer test; 18+9 assertions passed |
| VERI-04 | 128-05 | Unit tests for chromatindb_config_* gauge emission | SATISFIED | 3 [metrics][prometheus][config-gauge] TEST_CASEs; 43 assertions total including 24 existence + 14 exclusion + value fidelity + reload-scrape |

### Anti-Patterns Found

No blockers or stubs found. Detailed scan of modified files:

| File | Pattern Checked | Result |
|------|----------------|--------|
| `db/net/framing.h` | TODO/placeholder/empty impl | None found |
| `db/config/config.h` | stub default values | `4ULL * 1024 * 1024` is the functional default, not a stub |
| `db/config/config.cpp` | hardcoded ceiling literal | Zero hardcoded 64 MiB literal; all references use `MAX_BLOB_DATA_HARD_CEILING` symbol |
| `db/engine/engine.cpp` | remaining MAX_BLOB_DATA_SIZE | Zero — confirmed by grep of production source tree |
| `db/peer/peer_manager.cpp` | partial seeding (missing a path) | All 3 paths covered: constructor (lines 172-173), per-connection (lines 213, 229), SIGHUP (lines 582-589) |
| `db/peer/metrics_collector.cpp` | hardcoded values in gauge output | Zero — all 24 gauges use `std::to_string(config_.<field>)` |
| `db/peer/peer_manager.h` | const Config& (stale after SIGHUP) | Fixed — `config::Config config_` at line 150 (owned value) |
| Production source (non-test) | Any remaining MAX_BLOB_DATA_SIZE | Zero matches confirmed |
| Sync-out/read/GC paths | blob_max_bytes enforcement (D-16 violation) | Zero — only framing.h comment references; enforcement limited to engine.cpp, connection.cpp, message_dispatcher.cpp |

### Human Verification Required

None. All critical behaviors are verifiable programmatically. Targeted Catch2 tag runs confirmed in SUMMARY evidence. The full test suite run is user-delegated per project policy but does not block goal verification.

### Gaps Summary

No gaps. All 5 roadmap success criteria are fully satisfied with evidence confirmed in the actual codebase (not just SUMMARY claims). The phase goal — "Operators can set the blob cap in `config.json`, hot-reload it with SIGHUP, and verify it remotely via `/metrics`" — is achieved end-to-end:

1. `blob_max_bytes` in config.json parses, defaults to 4 MiB, and is bounds-validated at config load.
2. SIGHUP propagates the new cap to all three enforcement sites (engine ingest, chunked reassembly, NodeInfoResponse advertisement) and to every live connection.
3. MAX_FRAME_SIZE is 2 MiB with compile-time invariant protection.
4. 24 `chromatindb_config_*` gauges emit live values, updating after SIGHUP.
5. Tests cover all requirements with targeted tag runs passing (18+43+9 assertions).

The D-16 enforcement boundary is respected: sync-out, read, GC, and compaction paths are confirmed cap-free. Phase 129 scope is untouched.

---

_Verified: 2026-04-23T14:00:00Z_
_Verifier: Claude (gsd-verifier)_
