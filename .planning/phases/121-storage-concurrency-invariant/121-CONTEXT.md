# Phase 121: Storage Concurrency Invariant - Context

**Gathered:** 2026-04-19
**Status:** Ready for planning

<domain>
## Phase Boundary

Prove the implicit "everything that touches Storage runs on one io_context thread" model holds in practice across every reachable call path in `db/`. Where it doesn't, fix it before the Phase 122 schema change lands on top. Leave runtime regression-proofing so a future contributor can't silently reintroduce a race.

**This phase does NOT:**
- Add new storage features, indexes, or DBIs
- Redesign the concurrency model (no new thread pools, no new strands beyond storage-confinement if a strand is needed as the fix)
- Touch the wire format, signing canonical form, or anything Phase 122+ will touch
- Refactor `Storage` or `Engine` beyond what's needed to enforce the invariant

**This phase DOES:**
- Audit every db/ call path that reaches `Storage::*` methods
- Verify the co_await → thread_pool → post-back pattern is applied consistently
- Add a TSAN-enabled Catch2 unit test driving concurrent ingests through the actual dispatch path
- Add runtime thread-ID assertions at the top of every public `Storage` method
- Update `storage.h` and any affected comments to cite the enforcement mechanism
- Apply a fix ONLY IF the audit finds a real race — otherwise this phase is pure verification + assertion wiring

</domain>

<decisions>
## Implementation Decisions

### Verification Standard

- **D-01:** Proof-of-safety required before phase closes: code-read trace of every db/ path that reaches Storage PLUS one focused Catch2 unit test that drives concurrent ingests through the real dispatch path with TSAN enabled. If TSAN reports clean AND the trace shows every co_await boundary posts back to ioc_ before touching Storage, the invariant is considered proven.
- **D-02:** Code-read alone is not sufficient. Runtime evidence (TSAN) is the ship gate. No exhaustive stress/multi-peer tests required — one solid concurrent-ingest test is the bar.

### Audit Scope

- **D-03:** Trace every call path in `db/` that reaches `Storage::*`, not just the ingest path flagged in the audit. The explicit list:
  - **Ingest:** `db/engine/engine.cpp` (store_blob via handle_write, handle_delegation, handle_pubkey)
  - **Sync:** `db/sync/sync_protocol.h`, `db/sync/reconciliation.*` (hash enumeration, range splits, blob fetch)
  - **PEX:** `db/peer/pex_manager.cpp` (peer cursor reads/writes)
  - **Expiry:** `db/peer/sync_orchestrator.cpp` `expiry_scan_loop` (Storage::run_expiry_scan)
  - **Cursor updates:** any path that writes cursor DBI
  - **Peer manager:** `db/peer/peer_manager.cpp` (pubkey lookup, delegation resolution, tombstone queries)
  - **Startup:** `db/main.cpp` + cleanup_stale_cursors
- **D-04:** For each path, record on which thread/executor the Storage call runs. If it runs on anything other than the single io_context thread, flag it and apply the fix from D-06.

### Fix Mechanism (Conditional)

- **D-05:** Fix is conditional — applied ONLY if the audit finds a path that reaches Storage from a thread other than the main io_context. If every path is already post-back-to-ioc, no fix is needed and no mechanism is chosen.
- **D-06:** IF a fix is needed: the mechanism is to be selected during planning after the audit results are in. Preferred direction (but not locked): explicit `asio::strand` confinement of Storage (matches the existing "strand-confined" pattern at `db/peer/peer_types.h:58`). Mutex on Storage and aggressive restructuring are both fallbacks, not defaults. Re-surface to the user during planning if the audit uncovers a race and the fix shape isn't obvious.

### Regression-Proofing

- **D-07:** Every public `Storage::*` method gets a runtime thread-ID assertion at the top: the first call on an instance captures `std::this_thread::get_id()` as the expected TID; subsequent calls `assert` the current thread matches. Catches violations immediately in dev/test builds, compiles out under `NDEBUG` in release.
- **D-08:** Implementation detail (Claude's discretion during planning): choose between (a) a lightweight macro like `STORAGE_THREAD_CHECK()` at the top of each method, (b) a small RAII guard constructed at method entry, or (c) a helper `this->assert_owner_thread()`. Pick whichever reads cleanest given the existing Storage API surface.
- **D-09:** The assertion mechanism must not leak into Storage's public API — no token parameters, no templated executor types. Purely internal enforcement.

### Claude's Discretion

- Exact shape of the TSAN test (number of concurrent connections, blob count, assertion density) — pick whatever's sufficient to demonstrate the invariant holds.
- Layout of the assertion helper (macro vs RAII vs method) — pick cleanest fit.
- Whether to use spdlog to log the captured expected-TID at Storage construction for debug builds — discretion.
- Whether the thread-ID check needs relaxation for explicit startup paths where `Storage` is constructed before the io_context runs (e.g., cleanup_stale_cursors at main.cpp) — handle during planning.

</decisions>

<specifics>
## Specific Ideas

- **Existing patterns to follow:**
  - `db/peer/peer_types.h:58` already documents "strand-confined to io_context thread" counters — the vocabulary and mental model are established in the project.
  - `db/crypto/thread_pool.h:26-31` documents the `offload → post-back` idiom: `co_await offload(pool, f); co_await asio::post(ioc, use_awaitable);`. Every path that offloads must post back before touching Storage. This is the exact invariant the audit verifies.
  - Grep results show the post-back pattern is applied in `message_dispatcher.cpp` (lines 339, 1351), `blob_push_manager.cpp:194`, `pex_manager.cpp:358`, `connection_manager.cpp:300`, `sync_orchestrator.cpp:94`. These are the sites that look correct. The audit must confirm there are no missed co_await boundaries upstream of Storage calls.

- **Thread-ID assertion:** analogous patterns exist in other ASIO-based codebases. Simple `std::thread::id expected_tid_;` captured on first method invocation plus `assert(std::this_thread::get_id() == expected_tid_)` at each entry is the idiom. No need for atomics or synchronization on the TID itself — the whole point is that Storage is touched from exactly one thread.

- **TSAN build:** project already ships with a `sanitizers/` directory — TSAN is presumably already configured as a build flavor. Planning should confirm this and reuse the existing TSAN build target.

- **User explicit instruction:** fix mechanism is conditional — do NOT preemptively add a strand or mutex if the audit shows everything is already safe. The regression-proofing (thread-ID assertion) lands regardless.

</specifics>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

- `.planning/ROADMAP.md` — Phase 121 goal and success criteria
- `.planning/phases/121-storage-concurrency-invariant/121-CONTEXT.md` — this file
- `db/storage/storage.h` (line 104 — "NOT thread-safe" comment, the invariant being enforced)
- `db/engine/engine.h` (line 71 — same comment)
- `db/peer/peer_manager.h` (line 42 — "Runs on single io_context thread")
- `db/sync/sync_protocol.h` (line 35 — same)
- `db/peer/peer_types.h` (line 58 — "strand-confined to io_context thread" prior art)
- `db/crypto/thread_pool.h` (lines 26-31 — documented offload + post-back pattern)
- `sanitizers/` (for TSAN build configuration — reuse don't reinvent)

**Project memory references (from auto-memory):**

- `feedback_no_duplicate_code.md` — utilities go into shared headers, not copy-pasted
- `feedback_self_verify_checkpoints.md` — run live verification yourself when infra is reachable
- User preference: "Pick the right fix the first time" — no band-aid if the audit finds a race

</canonical_refs>

<deferred>
## Deferred Ideas

- **Full strand refactor / per-component strands** — if the audit reveals deep concurrency issues requiring a redesign (multiple concurrent io_context threads, per-connection strands intentional for throughput, etc.), that's a separate architecture phase and belongs in backlog.
- **Compile-time enforcement via Token<StorageThread> type pattern** — more rigorous than runtime assertions but adds C++ template machinery and touches every Storage call site. Overkill for now; backlog if runtime assertions prove insufficient.
- **Multi-peer sync stress test** — covered by Phase 124's live-node E2E, not needed here. This phase verifies the invariant; real-world concurrent load is validated end-to-end in Phase 124.
- **Performance profiling of the post-back pattern under load** — if there's a throughput concern with the current single-io_context model, it's a post-MVP optimization. Not Phase 121's problem.

</deferred>
