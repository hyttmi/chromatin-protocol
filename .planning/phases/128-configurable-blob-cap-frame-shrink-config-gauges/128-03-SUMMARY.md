---
phase: 128-configurable-blob-cap-frame-shrink-config-gauges
plan: 03
subsystem: engine, net, peer, runtime-wiring
tags: [blob-cap, sighup, runtime-wiring, ingest, chunked-reassembly, nodeinfo]
requires:
  - "Config::blob_max_bytes u64 field + validator (plans 128-01 + 128-02)"
  - "MAX_BLOB_DATA_HARD_CEILING rename (plan 128-01)"
  - "PeerManager::config_ owned-value refactor (plan 128-04)"
provides:
  - "BlobEngine::blob_max_bytes_ member + inline set_blob_max_bytes setter"
  - "MessageDispatcher::blob_max_bytes_ member + inline set_blob_max_bytes setter"
  - "Connection::blob_max_bytes_ per-connection member + inline set_blob_max_bytes setter"
  - "PeerManager seeds engine + dispatcher at construct AND on SIGHUP"
  - "PeerManager seeds every new connection at the server.on_connected chokepoint (TCP + UDS)"
  - "PeerManager iterates conn_mgr_.peers() on SIGHUP and pushes live cap to each connection"
  - "Ingest rejection message names the live cap (BLOB-04 / D-17)"
  - "Zero MAX_BLOB_DATA_SIZE references remain in production (non-test) source"
  - "chromatindb target builds green (Wave 3 re-greens the tree)"
affects:
  - db/engine/engine.h (BLOB-04 rejection disposition doc updated)
  - db/engine/engine.cpp (Step 0 size check + rejection message)
  - db/peer/message_dispatcher.h (new setter + member)
  - db/peer/message_dispatcher.cpp (NodeInfoResponse encoder reads seeded member)
  - db/peer/peer_manager.cpp (initial seed + on_connected chokepoints + SIGHUP reload)
  - db/net/connection.h (per-connection setter + member)
  - db/net/connection.cpp (chunked reassembly cap check reads seeded member)
  - cli/src/commands.cpp (comment refresh — no behavior change)
tech-stack:
  added: []
  patterns:
    - "Phase-127 member-seeding mirror: plain u64 member + inline setter + PeerManager-driven seeding at construct/SIGHUP (D-01)"
    - "Chokepoint per-connection cap seed in server.on_connected / uds_acceptor.on_connected lambdas — no ConnectionManager API change"
    - "Owned-value config_ (from 128-04) as the single source for SIGHUP re-seed + new-connection seed"
    - "Rejection message reads live cap member so operator sees post-SIGHUP enforcement level (D-17)"
key-files:
  created:
    - .planning/phases/128-configurable-blob-cap-frame-shrink-config-gauges/128-03-SUMMARY.md
  modified:
    - db/engine/engine.h
    - db/engine/engine.cpp
    - db/peer/message_dispatcher.h
    - db/peer/message_dispatcher.cpp
    - db/peer/peer_manager.cpp
    - db/net/connection.h
    - db/net/connection.cpp
    - cli/src/commands.cpp
decisions:
  - "Member + inline setter per D-01 — no atomic, no new RuntimeLimits struct, no constructor-signature churn. Mirrors set_rate_limits / set_max_subscriptions shape exactly."
  - "Chokepoint for per-connection seed is the server_.set_on_connected / uds_acceptor_->set_on_connected lambdas in PeerManager, NOT a new ConnectionManager API. These lambdas already capture 'this' and delegate to conn_mgr_.on_peer_connected after a one-line cap seed — zero additional API surface (option chosen per plan's executor-decision-rule)."
  - "Per-connection seed fires post-handshake, pre-message-loop (server.cpp wires it via conn->on_ready). This is the earliest moment at which a chunked frame could be accepted — so cap enforcement is in place before the first read."
  - "SIGHUP iterates conn_mgr_.peers() and pushes the new cap to every live connection so local enforcement is live mid-session. This is orthogonal to Phase 129's session-constant advertised peer cap — local enforcement lives in the Connection::blob_max_bytes_ member; peer advertisement snapshot is Phase 129's concern and is NOT affected here."
  - "CLI commands.cpp:673 comment updated from '500 MiB' to 'Config::blob_max_bytes (default 4 MiB, advertised via NodeInfoResponse)' — comment-only refresh so CLI documentation stays aligned with the node's new runtime shape."
  - "engine.h doc comment on IngestError::oversized_blob updated to reference Config::blob_max_bytes rather than the deleted MAX_BLOB_DATA_SIZE symbol (consistency)."
metrics:
  duration: "~7m"
  completed: 2026-04-22
  tasks-completed: 3
  files-modified: 8
  commits: 3
---

# Phase 128 Plan 03: Runtime Wiring of blob_max_bytes Summary

**One-liner:** Wires Config::blob_max_bytes end-to-end: BlobEngine / MessageDispatcher / Connection each own a seeded blob_max_bytes_ member + setter; PeerManager seeds engine + dispatcher at construct + SIGHUP and pushes the cap to every live connection on SIGHUP plus new connections via the server on_connected chokepoint. Zero MAX_BLOB_DATA_SIZE refs remain in production source and `chromatindb` builds green.

## What Landed

### Commit 1 (`0ef2ec08`): Member + setter on 3 owning objects

Files: `db/engine/engine.h`, `db/peer/message_dispatcher.h`, `db/net/connection.h`.

Added to each:
- Public inline setter `void set_blob_max_bytes(uint64_t cap) { blob_max_bytes_ = cap; }`
- Private `uint64_t blob_max_bytes_ = 4ULL * 1024 * 1024;` member (4 MiB default matches Config::blob_max_bytes default from 128-01)

No `.cpp` bodies added — all setters are inline trivial assignments, mirroring `set_max_subscriptions` at message_dispatcher.h:61 and `set_max_ttl_seconds` at engine.h:108. The BlobEngine constructor signature is unchanged per D-01/D-02; seeding happens via the setter, same post-hoc pattern Phase 127 uses for `max_subscriptions_`.

### Commit 2 (`c732ee47`): Callsite swap + IngestError live-cap message (D-17)

Files: `db/engine/engine.cpp`, `db/engine/engine.h` (comment), `db/net/connection.cpp`, `db/peer/message_dispatcher.cpp`, `cli/src/commands.cpp` (comment).

- `db/engine/engine.cpp:112` — Step 0 oversize check reads `blob_max_bytes_` instead of `net::MAX_BLOB_DATA_SIZE`. Rejection message formats `std::to_string(blob_max_bytes_)` so operators see the live cap in error text (BLOB-04 / D-17). Source comments anchor `BLOB-01/BLOB-04` + `D-17` per source-comment carve-out in `feedback_no_phase_leaks_in_user_strings.md` (internal comments only, not user-visible text).
- `db/engine/engine.h:30` — `IngestError::oversized_blob` doc comment updated to reference `Config::blob_max_bytes (live cap)` rather than the deleted `MAX_BLOB_DATA_SIZE` symbol.
- `db/net/connection.cpp:855` — chunked reassembly declared-size check reads `blob_max_bytes_`. Log message says "blob_max_bytes" to match config.json field name for operator-log grep-consistency.
- `db/peer/message_dispatcher.cpp:721` — NodeInfoResponse encoder advertises the live seeded `blob_max_bytes_` per D-04. The adjacent NODEINFO-02 write at line 725 (`MAX_FRAME_SIZE`) is unchanged — frame size stays a compile-time constant per D-10/FRAME-02.
- `cli/src/commands.cpp:673` — comment refresh: "Node enforces Config::blob_max_bytes (default 4 MiB, advertised via NodeInfoResponse)" replaces the outdated "Node enforces MAX_BLOB_DATA_SIZE (default 500 MiB)".

After this commit, zero `MAX_BLOB_DATA_SIZE` references remain in `db/` or `cli/` production source. Test-tree refs (test_framing.cpp, test_engine.cpp, test_peer_manager.cpp) are out of scope here and remain for plan 128-05.

### Commit 3 (`faea8dcb`): PeerManager seeding — construct + on_connected chokepoint + SIGHUP

File: `db/peer/peer_manager.cpp`.

Three seeding paths added, all reading from `config.blob_max_bytes` (construct) or `new_cfg.blob_max_bytes` (reload) or `config_.blob_max_bytes` (per-connection, via owned member from 128-04):

1. **Constructor body (lines 172-173)** — directly after the existing `dispatcher_.set_rate_limits` / `dispatcher_.set_max_subscriptions` calls:
   ```cpp
   engine_.set_blob_max_bytes(config.blob_max_bytes);
   dispatcher_.set_blob_max_bytes(config.blob_max_bytes);
   ```

2. **Per-connection seed (lines 213, 229)** — inside the `server_.set_on_connected` and `uds_acceptor_->set_on_connected` lambdas, before delegating to `conn_mgr_.on_peer_connected(conn)`:
   ```cpp
   conn->set_blob_max_bytes(config_.blob_max_bytes);
   ```
   `config_` is the owned `Config` member (refactored in plan 128-04); this ensures every new connection — both TCP and UDS — sees the current cap, even post-SIGHUP. Fires post-handshake, pre-message-loop via Connection::`ready_cb_` (server.cpp:143, 200, 308 `conn->on_ready([this]{...})`), so enforcement is in place before the first chunked frame can arrive.

3. **SIGHUP reload (lines 582-589)** — inside `reload_config`, directly after the `set_max_subscriptions` block:
   ```cpp
   engine_.set_blob_max_bytes(new_cfg.blob_max_bytes);
   dispatcher_.set_blob_max_bytes(new_cfg.blob_max_bytes);
   for (auto& peer : conn_mgr_.peers()) {
       if (peer && peer->connection) {
           peer->connection->set_blob_max_bytes(new_cfg.blob_max_bytes);
       }
   }
   spdlog::info("config reload: blob_max_bytes={}B", new_cfg.blob_max_bytes);
   ```
   The per-connection push makes local enforcement live mid-session. This is distinct from Phase 129's session-constant advertised peer cap (handshake snapshot) — local Connection enforcement state vs peer-capability snapshot state are two different things.

### Build acceptance (final Wave 3 gate)

`cmake --build build-debug -j$(nproc) --target chromatindb` → **exit 0**. `build-debug/chromatindb` binary linked at ~40 MB. The tree that Waves 1 and 2 left transiently red is now fully green. Plan 128-05 owns the test-tree migration; that target (`chromatindb_tests` / `cli_tests`) remains broken as expected per the phase's wave architecture.

## Acceptance Criteria (all pass)

### Task 1 — Member + setter adds
- AC1: `set_blob_max_bytes` setter + member in engine.h — PASS (lines 108, 196)
- AC2: same in message_dispatcher.h — PASS (lines 62, 88)
- AC3: same in connection.h — PASS (lines 127, 240)
- AC4: no `.cpp` bodies (setters are inline only) — PASS (0 matches in all 3 .cpp files)

### Task 2 — Callsite swap + message update
- AC1: `blob.data.size() > blob_max_bytes_` in engine.cpp — PASS (line 112)
- AC2: rejection uses `std::to_string(blob_max_bytes_)` — PASS (line 117)
- AC3: engine.cpp `MAX_BLOB_DATA_SIZE` count == 0 — PASS
- AC4: `total_payload_size > blob_max_bytes_` in connection.cpp — PASS (line 855)
- AC5: connection.cpp `MAX_BLOB_DATA_SIZE` count == 0 — PASS
- AC6: `store_u64_be(response.data() + off, blob_max_bytes_)` in dispatcher.cpp — PASS (line 721)
- AC7: `store_u32_be(..., chromatindb::net::MAX_FRAME_SIZE)` preserved — PASS (line 725)
- AC8: dispatcher.cpp `MAX_BLOB_DATA_SIZE` count == 0 — PASS
- AC9: production tree (excluding tests) has 0 `MAX_BLOB_DATA_SIZE` — PASS (empty leftover log)
- AC10: CLI `Node enforces Config::blob_max_bytes` comment — PASS (line 673)
- AC11: `cmake --build build-debug -j$(nproc) --target chromatindb` exit 0 — PASS (verified after Task 3 lands)

### Task 3 — Seeding
- AC1: constructor `engine_.set_blob_max_bytes(config.blob_max_bytes)` + dispatcher version — PASS (lines 172, 173)
- AC2: SIGHUP `engine_.set_blob_max_bytes(new_cfg.blob_max_bytes)` + dispatcher version — PASS (lines 582, 583)
- AC3: total `set_blob_max_bytes(new_cfg.blob_max_bytes)` count == 3 (engine + dispatcher + connection-loop) — PASS
- AC4: `config reload: blob_max_bytes=` spdlog line — PASS (line 589)
- AC5: total `set_blob_max_bytes` calls in db/peer and db/net cpp files ≥ 6 — PASS (count = 7: 2 construct + 2 on_connected + 3 SIGHUP)
- AC6: chromatindb builds green — PASS (exit 0)

## Deviations from Plan

### None (executor-decision-rule applied, not a deviation)

Task 3's `<action>` block flagged a single non-mechanical decision: where to hook per-connection seeding. The plan listed three options (chokepoint in ConnectionManager; per-factory-call fallback; etc.) and directed the executor to "use the chokepoint if one exists; otherwise fall back to per-factory-call." The chosen chokepoint is the **server_.set_on_connected / uds_acceptor_->set_on_connected lambdas already living in PeerManager's constructor body** (not a new ConnectionManager API).

Rationale (documented in Commit 3's body):
- These lambdas already capture `this` and delegate to `conn_mgr_.on_peer_connected(conn)`. Adding one line to seed the cap before delegation is surgical — zero ConnectionManager API change, zero new member on ConnectionManager, zero new wiring downstream.
- Both transports are covered: TCP (line 213) and UDS (line 229).
- Timing: these lambdas fire post-handshake, pre-message-loop (wired via `conn->on_ready` in `db/net/server.cpp:143, 200, 308` — verified during planning). This is the earliest moment at which Connection::handle_chunked_data could be called, so enforcement is in place before the first chunked frame can arrive.
- The plan's `<verify>` AC5 only requires ≥6 calls total and at least one new-connection seed site; the chokepoint approach provides both TCP and UDS seed sites, exceeding the floor.

This is an exercise of the plan's explicit "executor decision rule," not a deviation.

### No Rule 1/2/3 auto-fixes triggered

No pre-existing bugs surfaced during execution. No missing validation. No blocking issues. The plan's action blocks mapped cleanly onto the existing code — zero surprises.

## No Stubs

Every edit is live wired code:
- The 3 setters each assign a real runtime u64 with a sensible 4 MiB default.
- The 3 swapped callsites each read the real seeded member.
- The 3 seeding paths each pull from real Config fields (`config.blob_max_bytes`, `config_.blob_max_bytes`, `new_cfg.blob_max_bytes`).
- The ingest rejection message reads `blob_max_bytes_` via `std::to_string` at the time the error fires — operator sees the live value.

No placeholder data, no TODOs, no disabled code.

## Threat Flags

No new surface beyond the plan's `<threat_model>`:

- T-128-03-01 (Tampering — oversize blob at ingest): mitigate. Step 0 check rejects with `IngestError::oversized_blob` before any crypto offload. Message text now names the live cap (D-17).
- T-128-03-02 (DoS — chunked stream declaring > cap): mitigate. `Connection::handle_chunked_data` rejects at header parse time before allocating payload — attacker pays one round-trip, node pays ~14 bytes of parse work.
- T-128-03-03 (DoS — SIGHUP mid-stream): accept (documented). Chunked reassembly size check is per-stream at header parse time; pre-SIGHUP streams whose total_payload_size was already accepted complete without re-checking. Acceptable because SIGHUP is a rare operator action and stream lifetimes are O(seconds). Per-chunk accumulation safety net (line 885) is unchanged and still bounds accumulated bytes against the declared size.
- T-128-03-04 (Info Disclosure — NodeInfoResponse advertises live cap): accept per Phase 127 D-04 (public-by-design).
- T-128-03-05 (EoP — lie about chunked total): mitigate. Pre-existing check at db/net/connection.cpp:885 (`payload.size() > static_cast<size_t>(total_payload_size) + 64`) is unchanged by this plan.
- T-128-03-06 (Transport-level AEAD / streaming boundary): accept — already locked by Phase 126's audit.

No new threat flags to raise.

## Requirements Touched

- `BLOB-01` (operator-tunable blob cap) — **runtime wiring complete.** Field (128-01) + validator (128-02) + runtime read (128-03 this plan) + SIGHUP path (128-03 this plan) all live. Advertised cap (NodeInfoResponse) reads live seeded member. **BLOB-01 is now fully satisfied.**
- `BLOB-03` (SIGHUP-reloadable; new writes honor new cap) — **complete.** Engine Step 0 reads `blob_max_bytes_`, SIGHUP pushes new cap to engine + dispatcher + every live connection. Mid-session new writes (whether direct-ingest or chunked) see the post-SIGHUP cap. Pre-SIGHUP in-flight streams that already passed the header check are permitted to complete per T-128-03-03 disposition.
- `BLOB-04` (error message reflects live cap) — **complete.** Rejection message reads `std::to_string(blob_max_bytes_)` at throw time; no stale constants baked in.

## Commits

| Task | Commit     | Files | Summary |
|------|------------|-------|---------|
| 1    | `0ef2ec08` | `db/engine/engine.h`, `db/peer/message_dispatcher.h`, `db/net/connection.h` | Add `blob_max_bytes_` member + inline `set_blob_max_bytes` setter on three owning objects (BLOB-01; D-01/D-02 pattern). |
| 2    | `c732ee47` | `db/engine/engine.{cpp,h}`, `db/net/connection.cpp`, `db/peer/message_dispatcher.cpp`, `cli/src/commands.cpp` | Swap 3 runtime callsites + IngestError message to read seeded member (BLOB-01/BLOB-04 runtime halves; D-04/D-15/D-17). |
| 3    | `faea8dcb` | `db/peer/peer_manager.cpp` | Seed engine + dispatcher at construct and SIGHUP; per-connection seed at server.on_connected chokepoint (TCP + UDS); iterate live peers on SIGHUP (BLOB-01/BLOB-03; D-01 mirror of Phase 127). |

## Self-Check: PASSED

- `db/engine/engine.h` — FOUND, modified in `0ef2ec08` + `c732ee47`
- `db/engine/engine.cpp` — FOUND, modified in `c732ee47`
- `db/peer/message_dispatcher.h` — FOUND, modified in `0ef2ec08`
- `db/peer/message_dispatcher.cpp` — FOUND, modified in `c732ee47`
- `db/peer/peer_manager.cpp` — FOUND, modified in `faea8dcb`
- `db/net/connection.h` — FOUND, modified in `0ef2ec08`
- `db/net/connection.cpp` — FOUND, modified in `c732ee47`
- `cli/src/commands.cpp` — FOUND, modified in `c732ee47`
- Commit `0ef2ec08` — present in `git log`
- Commit `c732ee47` — present in `git log`
- Commit `faea8dcb` — present in `git log`
- `cmake --build build-debug -j$(nproc) --target chromatindb` — exit 0, `build-debug/chromatindb` executable linked (40519984 bytes)
- Grep sweep: zero `MAX_BLOB_DATA_SIZE` in `db/ cli/ --include='*.cpp' --include='*.h'` excluding tests — confirmed via `/tmp/128-03-leftover.log` empty file
- Scope compliance: only files listed in plan's `files_modified` frontmatter were touched (engine.{h,cpp}, message_dispatcher.{h,cpp}, peer_manager.cpp, connection.{h,cpp}, commands.cpp); STATE.md, ROADMAP.md, REQUIREMENTS.md untouched per parallel-executor directive

---
*Phase: 128-configurable-blob-cap-frame-shrink-config-gauges*
*Plan: 03 (runtime wiring — Wave 3 re-greens the tree)*
*Completed: 2026-04-22*
