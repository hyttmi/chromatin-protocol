---
phase: 128-configurable-blob-cap-frame-shrink-config-gauges
plan: 05
subsystem: tests, verification
tags: [tests, verification, config, metrics, engine, framing, sighup, blob-cap]
requires:
  - "Config::blob_max_bytes field + validator (plans 128-01 + 128-02)"
  - "BlobEngine::set_blob_max_bytes setter + runtime enforcement (plan 128-03)"
  - "chromatindb_config_* gauge emission + PeerManager owned-config_ refactor (plan 128-04)"
  - "MAX_BLOB_DATA_HARD_CEILING symbol + MAX_FRAME_SIZE = 2 MiB (plan 128-01)"
provides:
  - "VERI-01 coverage: 5 [config][blob-cap] TEST_CASEs (default, JSON parse, lower-bound reject, upper-bound reject, accept-at-boundaries)"
  - "VERI-01 SIGHUP coverage: 1 [config][blob-cap][sighup] engine-layer TEST_CASE exercising BlobEngine::set_blob_max_bytes"
  - "VERI-04 coverage: 2 [metrics][prometheus][config-gauge] TEST_CASEs (24 gauges + 14 exclusions; value fidelity at construction)"
  - "VERI-01 + METRICS-02 coverage: 1 [metrics][prometheus][config-gauge][sighup] TEST_CASE (pre-reload + post-reload scrape assertions across pm.reload_config())"
  - "Test migrations: zero MAX_BLOB_DATA_SIZE references in db/tests/; test_framing.cpp asserts new constants (2 MiB frame, 64 MiB ceiling); test_peer_manager.cpp NodeInfoResponse reads cfg.blob_max_bytes"
  - "chromatindb_tests target re-greened (wave architecture closure)"
affects:
  - db/tests/config/test_config.cpp (6 new TEST_CASEs; 6 include additions)
  - db/tests/peer/test_metrics_endpoint.cpp (3 new TEST_CASEs; new helper; <fstream>/<filesystem> includes)
  - db/tests/engine/test_engine.cpp (oversized_blob SECTION: explicit cap seed + symbol migration)
  - db/tests/net/test_framing.cpp (Protocol constants TEST_CASE: new symbol + 2 MiB frame assertions)
  - db/tests/peer/test_peer_manager.cpp (NodeInfoResponse assertion reads live Config::blob_max_bytes)
tech-stack:
  added: []
  patterns:
    - "Public-method reload test pattern (call reload_config() directly rather than raise(SIGHUP))"
    - "In-place config.json rewrite + nullary reload_config() mirrors production SIGHUP-on-disk operator flow"
    - "Explicit engine.set_blob_max_bytes seed in oversized-blob tests to decouple from Config default (64 MiB cap allocation vs 500 MiB)"
    - "get_prometheus_text_with_cfg(const Config&) sibling helper overrides bind_address/data_dir while preserving caller's numeric-field values"
key-files:
  created:
    - .planning/phases/128-configurable-blob-cap-frame-shrink-config-gauges/128-05-SUMMARY.md
  modified:
    - db/tests/config/test_config.cpp
    - db/tests/peer/test_metrics_endpoint.cpp
    - db/tests/engine/test_engine.cpp
    - db/tests/net/test_framing.cpp
    - db/tests/peer/test_peer_manager.cpp
decisions:
  - "reload_config() called directly (not raise(SIGHUP)) per plan's explicit decision — signal dispatch is validated in phase 118 infrastructure; reload_config is the public method SIGHUP triggers in production."
  - "SIGHUP reload test rewrites the SAME config.json file because peer_manager.cpp:514 reload_config() reads from the fixed config_path_ member (nullary signature) — rewrite-in-place mirrors the production operator workflow."
  - "Fresh PeerManager constructed inline in the reload test (not via a helper) because reload_config operates on a live PeerManager across two scrapes; a helper returning a string would tear down the object between calls."
  - "oversized_blob tests explicitly seed engine.set_blob_max_bytes(MAX_BLOB_DATA_HARD_CEILING) so tests allocate 64 MiB not the old 500 MiB, saving runtime + memory while preserving the semantic (oversize rejection) under a known cap independent of the 4 MiB Config default."
  - "test_peer_manager.cpp NodeInfoResponse byte-exact test reads cfg.blob_max_bytes (in-scope local Config) rather than a hardcoded literal — future Config-default changes automatically cascade."
  - "Non-trivial Rule 3 auto-fix: missing <fstream>/<filesystem> includes needed by the new reload test bodies; added alongside <asio.hpp> in the include block."
metrics:
  duration: "~20m"
  completed: 2026-04-23
  tasks-completed: 3
  files-modified: 5
  commits: 3
---

# Phase 128 Plan 05: VERI-01 + VERI-04 Test Coverage + Wave-3 Test Migration Summary

**One-liner:** 3 [config][blob-cap] bounds tests + 1 [config][blob-cap][sighup] engine-reload test + 3 [metrics][prometheus][config-gauge] tests (one of which carries [sighup]), plus pre-plan-128 test migrations off MAX_BLOB_DATA_SIZE / 110 MiB MAX_FRAME_SIZE literals, re-greening chromatindb_tests.

## What Landed

### Commit 1 (`75cfddf0`): Test migrations — re-green chromatindb_tests

Files: `db/tests/engine/test_engine.cpp`, `db/tests/net/test_framing.cpp`, `db/tests/peer/test_peer_manager.cpp`.

**test_engine.cpp oversized_blob SECTION block** (lines 474-551): Explicit `engine.set_blob_max_bytes(chromatindb::net::MAX_BLOB_DATA_HARD_CEILING)` seed injected after BlobEngine construction; 7 lines referencing the deleted `MAX_BLOB_DATA_SIZE` migrated to `MAX_BLOB_DATA_HARD_CEILING`. Allocations drop from 500 MiB to 64 MiB. SECTION titles updated from "...MAX_BLOB_DATA_SIZE..." to "...blob_max_bytes cap..." (internal test-identifier carve-out; not user-visible strings). The D-17 live-cap message assertion now reads `std::to_string(MAX_BLOB_DATA_HARD_CEILING + 1)` as the oversize detail-substring to match.

**test_framing.cpp "Protocol constants" TEST_CASE** (lines 186-205):
- SECTION `"MAX_BLOB_DATA_SIZE is 500 MiB"` → `"MAX_BLOB_DATA_HARD_CEILING is 64 MiB (Phase 128)"`. REQUIREs check `64ULL * 1024 * 1024` and `67108864ULL`.
- SECTION `"MAX_FRAME_SIZE is 110 MiB"` → `"MAX_FRAME_SIZE is 2 MiB (per-frame limit)"`. REQUIREs check `2u * 1024 * 1024` and `2097152u`.
- STREAMING_THRESHOLD + `MAX_FRAME_SIZE > STREAMING_THRESHOLD` sections unchanged (still hold).

**test_peer_manager.cpp NodeInfoResponse assertion** (line ~2900): `CHECK(max_blob_data_bytes == chromatindb::net::MAX_BLOB_DATA_SIZE)` → `CHECK(max_blob_data_bytes == cfg.blob_max_bytes)`. Source comment updated to reference "Config::blob_max_bytes seeded member per Phase 128 D-04". `max_frame_bytes` assertion unchanged — `MAX_FRAME_SIZE` still exists, just now equals 2 MiB (the symbol read is still correct).

### Commit 2 (`90a5c9a9`): VERI-01 config bounds + engine-layer SIGHUP enforcement

File: `db/tests/config/test_config.cpp` (6 new TEST_CASEs + 6 include additions).

Added includes: `db/engine/engine.h`, `db/net/framing.h`, `db/storage/storage.h`, `db/tests/test_helpers.h`, `<asio.hpp>`, `<cstring>`.

New TEST_CASEs:
1. `Config::blob_max_bytes default is 4 MiB` — struct-default pin (`cfg.blob_max_bytes == 4194304ULL`).
2. `load_config parses blob_max_bytes from JSON` — wire-path coverage (writes `{"blob_max_bytes": 8388608}` to tempfile, reads back 8 MiB).
3. `validate_config rejects blob_max_bytes below 1 MiB floor` — SECTIONs for `0` (rejects, message contains `"blob_max_bytes"` + `"1048576"`) and `1048575` (rejects).
4. `validate_config rejects blob_max_bytes above 64 MiB hard ceiling` — SECTIONs for `MAX_BLOB_DATA_HARD_CEILING + 1` (rejects) and `128ULL * 1024 * 1024` (rejects with "64 MiB" in message).
5. `validate_config accepts blob_max_bytes at boundaries and default` — SECTIONs for default 4 MiB, 1 MiB floor, 64 MiB ceiling, 8 MiB operator raise (all `REQUIRE_NOTHROW`).
6. `BlobEngine::set_blob_max_bytes lowers cap post-construction (SIGHUP reload path)` — tagged `[config][blob-cap][sighup]`. Three SECTIONs:
   - pre-reload: 3 MiB signed blob accepted under default 4 MiB cap.
   - post-reload: after `engine.set_blob_max_bytes(2 MiB)`, same 3 MiB blob rejected with `IngestError::oversized_blob`; D-17 message contains `"2097152"` (the live 2 MiB cap in bytes).
   - post-reload-raise: after `engine.set_blob_max_bytes(16 MiB)`, 3 MiB blob re-accepted.

### Commit 3 (`b32eb4a2`): VERI-04 gauges + VERI-01/METRICS-02 reload-scrape fidelity

File: `db/tests/peer/test_metrics_endpoint.cpp` (3 new TEST_CASEs + new helper + 2 new includes).

Added includes: `<filesystem>`, `<fstream>`.

New helper `get_prometheus_text_with_cfg(const Config& cfg_override)` constructed alongside the existing `get_prometheus_text()` in the anonymous namespace. Accepts a caller-supplied Config and forces test-only `bind_address = "127.0.0.1:0"` + `data_dir = tmp.path.string()` regardless of caller input (environment-specific fields, not value-fidelity subjects).

New TEST_CASEs:
1. `prometheus_metrics_text contains chromatindb_config_* gauges for all numeric Config fields` (`[metrics][prometheus][config-gauge]`) — asserts presence of all 24 alphabetically-ordered gauges (blob_max_bytes … worker_threads) and absence of 14 excluded fields per D-07 (bind_address, storage_path, data_dir, log_level, log_format, log_file, uds_path, metrics_bind, and the 5 vector fields + namespace_quotas map). 38 matchers total.
2. `prometheus_metrics_text config gauge values reflect Config at PeerManager construction` (`[metrics][prometheus][config-gauge]`) — METRICS-01. Custom Config with `blob_max_bytes = 8388608` and `max_peers = 77`; asserts `chromatindb_config_blob_max_bytes 8388608\n` and `chromatindb_config_max_peers 77\n` in scraped text.
3. `reload_config changes config gauge values without node restart (VERI-01 SIGHUP + METRICS-02 live-scrape)` (`[metrics][prometheus][config-gauge][sighup]`) — single test covering BOTH VERI-01 SIGHUP reload AND METRICS-02 live-scrape-reflects-reload. Writes initial config.json with blob_max_bytes = 4 MiB, constructs PeerManager passing cfg_path as the 8th constructor arg, scrapes and asserts `chromatindb_config_blob_max_bytes 4194304\n`, rewrites the SAME config.json with 8 MiB, calls `pm.reload_config()`, scrapes again and asserts `chromatindb_config_blob_max_bytes 8388608\n` present + `...4194304\n` absent.

## Acceptance Criteria (all pass)

### Task 1 — Test migrations

| AC | Check | Result |
|----|-------|--------|
| AC1 | `grep 'MAX_BLOB_DATA_SIZE' db/tests/engine/test_engine.cpp` returns 0 | ✓ |
| AC2 | `grep 'MAX_BLOB_DATA_HARD_CEILING\|set_blob_max_bytes' db/tests/engine/test_engine.cpp` ≥ 1 | ✓ (multiple) |
| AC3 | `grep 'MAX_BLOB_DATA_HARD_CEILING == 64ULL' db/tests/net/test_framing.cpp` == 1 | ✓ |
| AC4 | `grep 'MAX_FRAME_SIZE == 2u \* 1024 \* 1024' db/tests/net/test_framing.cpp` == 1 | ✓ |
| AC5 | No `MAX_FRAME_SIZE == 110` or `115343360u` in test_framing.cpp | ✓ (0 matches each) |
| AC6 | No `MAX_BLOB_DATA_SIZE` in test_framing.cpp | ✓ (0 matches) |
| AC7 | `grep 'max_blob_data_bytes == cfg.blob_max_bytes'` in test_peer_manager.cpp ≥ 1 | ✓ |
| AC8 | No `MAX_BLOB_DATA_SIZE` in test_peer_manager.cpp | ✓ (0 matches) |
| AC9 | `cmake --build build-debug -j$(nproc) --target chromatindb_tests` exit 0 | ✓ |

### Task 2 — VERI-01 blob-cap tests

| AC | Check | Result |
|----|-------|--------|
| AC1 | 6 TEST_CASEs tagged `[config][blob-cap]` | ✓ (5 pure + 1 also-sighup) |
| AC2 | 1 TEST_CASE tagged `[sighup]` | ✓ |
| AC3-AC8 | 6 specific title strings present | ✓ (all match) |
| AC9 | ≥ 2 `engine.set_blob_max_bytes` calls | ✓ (2 matches, lowering + raising sections) |
| AC10 | ≥ 1 `IngestError::oversized_blob` match | ✓ |
| AC11 | `#include "db/net/framing.h"` present | ✓ |
| AC12 | `#include "db/engine/engine.h"` present | ✓ |
| AC13 | chromatindb_tests builds clean | ✓ |

### Task 3 — VERI-04 config-gauge + VERI-01 reload-scrape tests

| AC | Check | Result |
|----|-------|--------|
| AC1 | 3 TEST_CASEs `[metrics][prometheus][config-gauge]` | ✓ |
| AC2 | 1 TEST_CASE `[config-gauge][sighup]` | ✓ |
| AC3 | ≥ 40 `ContainsSubstring("chromatindb_config_` matchers | ✓ (43) |
| AC4a | ≥ 2 `chromatindb_config_blob_max_bytes 8388608` matches | ✓ (2) |
| AC4b | `chromatindb_config_max_peers 77` match | ✓ (1) |
| AC5 | `pm.reload_config()` call present | ✓ (1) |
| AC6 | ≥ 2 `chromatindb_config_blob_max_bytes 4194304` matches | ✓ (2, pre + absence-after) |
| AC7 | `PeerManager pm(...cfg_path)` construction | ✓ (1) |
| AC8 | ≥ 2 `get_prometheus_text_with_cfg` refs | ✓ (2, definition + call) |
| AC9 | chromatindb_tests builds clean | ✓ |

## must_haves truths (plan-level)

- ✓ A new [config][blob-cap] TEST_CASE asserts default is 4 MiB.
- ✓ A new [config][blob-cap] TEST_CASE asserts load_config parses blob_max_bytes from JSON.
- ✓ A new [config][blob-cap] TEST_CASE asserts validate_config rejects blob_max_bytes < 1 MiB.
- ✓ A new [config][blob-cap] TEST_CASE asserts validate_config rejects blob_max_bytes > 64 MiB.
- ✓ A new [config][blob-cap] TEST_CASE asserts validate_config accepts at both boundaries and default.
- ✓ A new [config][blob-cap][sighup] TEST_CASE exercises BlobEngine::set_blob_max_bytes and the post-reload enforcement path.
- ✓ A new [metrics][prometheus][config-gauge] TEST_CASE asserts presence + exclusion of the 24 gauges / 14 excluded fields.
- ✓ A new [metrics][prometheus][config-gauge] TEST_CASE asserts gauge values reflect Config at construction time.
- ✓ A new [metrics][prometheus][config-gauge][sighup] TEST_CASE asserts the post-reload gauge value changes across pm.reload_config().
- ✓ Pre-existing tests referencing MAX_BLOB_DATA_SIZE / MAX_FRAME_SIZE=110 MiB migrated or deleted.
- ✓ chromatindb_tests builds clean after migration.
- Targeted test-suite RUNs executed (permitted by plan guardrails for targeted tags): `[config][blob-cap]` 18 assertions / 6 cases PASSED; `[metrics][prometheus][config-gauge]` 43 assertions / 3 cases PASSED; `[sighup]` 9 assertions / 2 cases PASSED. Full-suite run remains USER-DELEGATED per `feedback_delegate_tests_to_user.md`.

## Deviations from Plan

### Rule 3 — Auto-fix blocking issues

**1. [Rule 3 - Blocking] Missing `<fstream>` + `<filesystem>` includes in test_metrics_endpoint.cpp**

- **Found during:** Task 3 first build attempt.
- **Issue:** The new reload test uses `std::ofstream` (via `f << ...`) and `std::filesystem::create_directories`. Compilation failed with "`std::ofstream` has incomplete type; `fstream` is defined in header `<fstream>`".
- **Fix:** Added `#include <filesystem>` and `#include <fstream>` next to the existing `#include <asio.hpp>` in test_metrics_endpoint.cpp.
- **Files modified:** `db/tests/peer/test_metrics_endpoint.cpp`.
- **Commit:** `b32eb4a2` (folded into the Task 3 commit).

**2. [Rule 3 - Blocking] Incorrect variable reference in reload test PeerManager construction**

- **Found during:** Task 3 code authorship self-check (grep AC7).
- **Issue:** Initial draft passed `cfg` (undefined in that scope) to `PeerManager pm(cfg, ...)`. The in-scope Config variable is `initial_cfg` (product of `load_config(cfg_path)`).
- **Fix:** Changed to `PeerManager pm(initial_cfg, ..., cfg_path);` before the first build attempt.
- **Files modified:** `db/tests/peer/test_metrics_endpoint.cpp`.
- **Commit:** `b32eb4a2` (folded into the Task 3 commit).

### Cosmetic — TEST_CASE line continuations

Three new TEST_CASE macros were initially drafted with the title on line N and the tag list on line N+1 (matching Catch2 style for long titles). The plan's ACs grep for `TEST_CASE.*\[config\]\[blob-cap\]` / `TEST_CASE.*\[config-gauge\]` on a single line, so each was collapsed to a single line to match. No semantic change; Catch2 parses identically either way.

### No Rule 1/2 fixes triggered

No pre-existing bugs surfaced. No missing validation discovered. All plan actions mapped cleanly onto production code from waves 1-3.

### No Rule 4 escalations

No architectural changes needed — the test infrastructure (Catch2 matchers, TempDir helper, BlobEngine+Storage ctor, PeerManager 8th arg, reload_config nullary signature) all support the new assertions without modification.

## Build Gate Status

`cmake --build build-debug -j$(nproc) --target chromatindb_tests` exits 0 after Task 1, Task 2, and Task 3 each in sequence. This is the Wave-4 re-greening of the target that plans 128-01 and 128-03 transiently broke pending migration.

Targeted test tag runs (permitted by plan guardrails):
- `./build-debug/db/chromatindb_tests "[config][blob-cap]"` → 18 assertions / 6 test cases PASSED.
- `./build-debug/db/chromatindb_tests "[metrics][prometheus][config-gauge]"` → 43 assertions / 3 test cases PASSED.
- `./build-debug/db/chromatindb_tests "[sighup]"` → 9 assertions / 2 test cases PASSED.

Full-suite run is USER-DELEGATED per `feedback_delegate_tests_to_user.md`.

## No Stubs

No placeholder data, no TODOs, no mocked values. Every new test either constructs a live Config, writes live JSON to tempfile, or seeds a live engine — and every assertion reads the live scrape output / live ingest result. The `three_mib_payload` is a `std::string(3 MiB, 0x42)` with a proper ML-DSA-87 signature via `make_signed_blob`; not a mock.

## Threat Flags

No new attack surface. Test code does not introduce production-facing threats; it mitigates regression of Phase 128 enforcement invariants:

- **T-128-05-01 (Regression — bounds check weakened):** mitigated by the 3 rejection TEST_CASEs + 1 acceptance TEST_CASE covering 8 boundary conditions.
- **T-128-05-02 (Regression — excluded field leaked):** mitigated by 14 negative-matcher assertions in the gauge-coverage TEST_CASE.
- **T-128-05-03 (Regression — cached Config):** mitigated by the value-fidelity TEST_CASE that seeds a non-default Config at construction and asserts the gauge reflects the seed.
- **T-128-05-04 (Regression — reload path broken):** mitigated by TWO reload tests — engine-layer (test_config.cpp [sighup]) AND PeerManager-layer (test_metrics_endpoint.cpp [config-gauge][sighup]) — pinning both halves of the Config → engine/dispatcher/gauge propagation.

## Requirements Touched

- `VERI-01` (operator-tunable blob_max_bytes tested end-to-end, including SIGHUP reload) — **complete.** 6 [blob-cap] TEST_CASEs on the config layer + 1 [blob-cap][sighup] TEST_CASE on the engine layer + 1 [config-gauge][sighup] TEST_CASE on the PeerManager layer together satisfy every sub-clause.
- `VERI-04` (chromatindb_config_* gauge coverage) — **complete.** 24 gauges existence + 14 exclusions + value fidelity at construction.
- `METRICS-01` (value-fidelity gauge emission) — **complete.** Construction-time TEST_CASE pins the live-read invariant.
- `METRICS-02` (live-read reflects post-SIGHUP Config) — **complete.** Reload scrape TEST_CASE asserts the post-reload gauge value and the absence of the pre-reload value.
- `BLOB-03` (SIGHUP-reloadable; new writes honor new cap) — **test coverage added.** Engine-level set_blob_max_bytes test locks the ingest-reject-on-post-reload-cap behavior; existing-blob preservation (read-path) is D-14 territory and not in this plan's scope.
- `FRAME-01` (MAX_FRAME_SIZE = 2 MiB) — **test assertions migrated.** test_framing.cpp now pins the 2 MiB invariant by REQUIRE rather than the old 110 MiB.

## Commits

| Task | Commit | Files | Summary |
|------|--------|-------|---------|
| 1 | `75cfddf0` | `db/tests/engine/test_engine.cpp`, `db/tests/net/test_framing.cpp`, `db/tests/peer/test_peer_manager.cpp` | Migrate pre-plan-128 tests off MAX_BLOB_DATA_SIZE and 110 MiB MAX_FRAME_SIZE literals |
| 2 | `90a5c9a9` | `db/tests/config/test_config.cpp` | Add [config][blob-cap] TEST_CASEs for VERI-01 (bounds, default, load_config parse, SIGHUP-reload engine enforcement) |
| 3 | `b32eb4a2` | `db/tests/peer/test_metrics_endpoint.cpp` | Add [metrics][prometheus][config-gauge] TEST_CASEs for VERI-04 + VERI-01 reload-scrape fidelity |

## Self-Check: PASSED

- `db/tests/engine/test_engine.cpp` — modified, in commit `75cfddf0` ✓
- `db/tests/net/test_framing.cpp` — modified, in commit `75cfddf0` ✓
- `db/tests/peer/test_peer_manager.cpp` — modified, in commit `75cfddf0` ✓
- `db/tests/config/test_config.cpp` — modified, in commit `90a5c9a9` ✓
- `db/tests/peer/test_metrics_endpoint.cpp` — modified, in commit `b32eb4a2` ✓
- Commit `75cfddf0` — present in `git log` ✓
- Commit `90a5c9a9` — present in `git log` ✓
- Commit `b32eb4a2` — present in `git log` ✓
- `cmake --build build-debug -j$(nproc) --target chromatindb_tests` exit 0 ✓
- Scope compliance: only files listed in plan's `files_modified` frontmatter touched; STATE.md / ROADMAP.md / REQUIREMENTS.md / production source untouched per parallel-executor directive ✓
- Zero MAX_BLOB_DATA_SIZE references in db/tests/ ✓
- Zero MAX_FRAME_SIZE == 110 / 115343360u references in db/tests/ ✓

---
*Phase: 128-configurable-blob-cap-frame-shrink-config-gauges*
*Plan: 05 (VERI-01 + VERI-04 test coverage — Wave 4 re-greens chromatindb_tests)*
*Completed: 2026-04-23*
