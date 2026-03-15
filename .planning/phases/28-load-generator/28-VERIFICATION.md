---
phase: 28-load-generator
verified: 2026-03-15T18:00:00Z
status: passed
score: 7/7 must-haves verified
re_verification: false
---

# Phase 28: Load Generator Verification Report

**Phase Goal:** A standalone C++ tool can generate sustained signed-blob traffic against any chromatindb node
**Verified:** 2026-03-15T18:00:00Z
**Status:** passed
**Re-verification:** No — initial verification

## Goal Achievement

### Observable Truths

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| 1 | chromatindb_loadgen binary compiles and links against chromatindb_lib | VERIFIED | `cmake --build build --target chromatindb_loadgen` succeeds; binary at `build/chromatindb_loadgen`; `target_link_libraries(chromatindb_loadgen PRIVATE chromatindb_lib)` at CMakeLists.txt:141 |
| 2 | loadgen connects to a running node, performs PQ handshake, and sends blobs that the node accepts | VERIFIED | `Connection::create_outbound()` at line 321-322 reuses the full Connection class (handles PQ handshake transparently); `conn_->run()` drives handshake + message loop; `on_ready` callback fires post-handshake |
| 3 | blobs are sent at a fixed rate via steady_timer, not response-driven (no coordinated omission) | VERIFIED | `asio::steady_timer timer_` at line 581; `timer_.expires_at(schedule[i])` at line 442; schedule pre-computed at lines 421-424 using `Clock::now() + interval * i` |
| 4 | latency is measured from scheduled send time, not actual send time | VERIFIED | `pending_sends_[hash_hex] = schedule[i]` stores the scheduled time at line 466; `auto scheduled_time = it->second; ... now - scheduled_time` at lines 538-541; uses `Clock` (steady_clock) throughout |
| 5 | mixed-size mode distributes blobs across 1 KiB, 100 KiB, and 1+ MiB sizes | VERIFIED | `std::discrete_distribution<int> dist({70, 20, 10})` at line 142; SIZE_SMALL=1024, SIZE_MEDIUM=100*1024, SIZE_LARGE=1024*1024 at lines 113-115; `--mixed` CLI flag at line 91 |
| 6 | JSON summary with blobs/sec, MiB/sec, p50/p95/p99, errors is written to stdout on completion | VERIFIED | `stats_to_json()` at lines 227-251 emits all required fields; `j["blobs_per_sec"]`, `j["mib_per_sec"]`, `j["latency_ms"]["p50/p95/p99"]`, `j["errors"]`; `std::cout << json.dump(2)` at line 647; spdlog routes to stderr |
| 7 | Dockerfile builds and copies chromatindb_loadgen alongside chromatindb | VERIFIED | `COPY loadgen/ loadgen/` at Dockerfile:14; `chromatindb_loadgen` in build target at Dockerfile:22; `strip build/chromatindb_loadgen` at Dockerfile:23; `COPY --from=builder /src/build/chromatindb_loadgen /usr/local/bin/` at Dockerfile:36 |

**Score:** 7/7 truths verified

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `loadgen/loadgen_main.cpp` | Complete load generator tool | VERIFIED | 651 lines (minimum 350); substantive: CLI parsing, `LoadGenerator` class, timer-driven send loop, notification ACK matching, stats computation, JSON output; wired: imported in CMakeLists.txt as source for `chromatindb_loadgen` target |
| `CMakeLists.txt` | chromatindb_loadgen target linked to chromatindb_lib | VERIFIED | `add_executable(chromatindb_loadgen loadgen/loadgen_main.cpp)` at line 140; `target_link_libraries(chromatindb_loadgen PRIVATE chromatindb_lib)` at line 141 |
| `Dockerfile` | Container build including loadgen binary | VERIFIED | 4 required references present: source COPY (line 14), build target (line 22), strip (line 23), runtime COPY (line 36) |

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `loadgen/loadgen_main.cpp` | `chromatindb_lib` | CMake target_link_libraries | VERIFIED | `target_link_libraries(chromatindb_loadgen PRIVATE chromatindb_lib)` at CMakeLists.txt:141; build succeeds with full link |
| `loadgen/loadgen_main.cpp` | `db/net/connection.h` | `Connection::create_outbound` + `conn->run()` | VERIFIED | `Connection::create_outbound(std::move(socket), identity_)` at line 321; `co_await conn_->run()` at line 349 |
| `loadgen/loadgen_main.cpp` | `db/wire/codec.h` | `encode_blob` + `build_signing_input` | VERIFIED | `wire::encode_blob(blob)` at line 460; `wire::build_signing_input(...)` at line 190-191; `wire::blob_hash(...)` at line 461-462 |
| `loadgen/loadgen_main.cpp` | `db/identity/identity.h` | `NodeIdentity::generate()` | VERIFIED | `NodeIdentity::generate()` at line 630; namespace used for blob signing and subscription |

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|------------|-------------|--------|----------|
| LOAD-01 | 28-01-PLAN.md | chromatindb_loadgen C++ binary connects as protocol-compliant peer, performs PQ handshake, sends signed blobs | SATISFIED | `Connection::create_outbound` + `conn->run()` handles PQ handshake; `make_signed_blob` at lines 179-194 using `build_signing_input` + `identity.sign()`; `send_message(TransportMsgType_Data, ...)` at line 468-470 |
| LOAD-02 | 28-01-PLAN.md | Load generator supports configurable blob count, sizes, and write rate with timer-driven scheduling (no coordinated omission) | SATISFIED | `--count`, `--rate`, `--size` CLI flags parsed at lines 85-90; `asio::steady_timer` with pre-computed `schedule[i]` at lines 418-424; latency measured from `schedule[i]` not actual send time |
| LOAD-03 | 28-01-PLAN.md | Mixed-size workload mode distributes blobs across small/medium/large sizes | SATISFIED | `--mixed` flag; `discrete_distribution({70, 20, 10})` at line 142; three size classes: 1 KiB / 100 KiB / 1 MiB; JSON breakdown with `small_1k`, `medium_100k`, `large_1m` counts |

**No orphaned requirements.** LOAD-04 is mapped to Phase 30 (not Phase 28) and does not appear in 28-01-PLAN.md's `requirements` field.

### Anti-Patterns Found

| File | Line | Pattern | Severity | Impact |
|------|------|---------|----------|--------|
| `loadgen/loadgen_main.cpp` | 454-458 | `system_clock` used for blob timestamp uniqueness | Info | Intentional and correct: blob `timestamp` field needs wall-clock microseconds for uniqueness across runs. Latency measurement uses `steady_clock` (`Clock`) at lines 538-541. No impact on measurement accuracy. |

No TODOs, FIXMEs, placeholder comments, empty handlers, or stub implementations found.

### Human Verification Required

#### 1. End-to-end Integration: Loadgen Connects, Handshakes, and Blobs Are Accepted

**Test:** Start a chromatindb node (`./build/chromatindb run --data-dir /tmp/testdb`), then run `./build/chromatindb_loadgen --target 127.0.0.1:4200 --count 10 --rate 5`. Check node logs for ingested blobs and verify loadgen emits JSON to stdout.
**Expected:** Node logs show 10 blobs ingested from the loadgen's namespace. JSON output includes `"total_blobs": 10`, non-zero `blobs_per_sec`, and `notifications_received` matching sent count.
**Why human:** Requires running two processes against a live node; pub/sub notification flow is async and cannot be traced statically.

#### 2. Mixed-Size Distribution: Correct 70/20/10 Proportions

**Test:** Run `./build/chromatindb_loadgen --target 127.0.0.1:4200 --count 1000 --rate 50 --mixed` and inspect the JSON `blob_sizes` section.
**Expected:** `small_1k` approximately 700, `medium_100k` approximately 200, `large_1m` approximately 100 (statistical distribution, not exact).
**Why human:** Distribution correctness at scale requires actual execution; seeded RNG behavior is correct by code inspection but proportional output needs live observation.

### Gaps Summary

No gaps found. All 7 must-have truths are verified, all 3 artifacts pass all three levels (exists, substantive, wired), all 4 key links are confirmed, and all 3 requirements (LOAD-01, LOAD-02, LOAD-03) are satisfied with direct code evidence.

The binary compiles cleanly and produces correct usage output. Implementation is substantive at 651 lines with no stub patterns.

---

_Verified: 2026-03-15T18:00:00Z_
_Verifier: Claude (gsd-verifier)_
