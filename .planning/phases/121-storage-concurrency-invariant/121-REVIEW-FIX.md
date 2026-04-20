---
phase: 121-storage-concurrency-invariant
fixed_at: 2026-04-19T00:00:00Z
review_path: .planning/phases/121-storage-concurrency-invariant/121-REVIEW.md
iteration: 1
findings_in_scope: 2
fixed: 2
skipped: 0
status: all_fixed
---

# Phase 121: Code Review Fix Report

**Fixed at:** 2026-04-19
**Source review:** .planning/phases/121-storage-concurrency-invariant/121-REVIEW.md
**Iteration:** 1

**Summary:**
- Findings in scope: 2 (Critical + Warning only; 6 Info findings deferred)
- Fixed: 2
- Skipped: 0

Build and targeted tests verified green after both fixes:
`cmake --build build -j$(nproc) --target chromatindb_tests` compiled
cleanly, and
`./build/db/chromatindb_tests "[storage],[engine],[sync],[thread_check]" --reporter compact`
reported "All tests passed (1473 assertions in 233 test cases)".

## Fixed Issues

### WR-01: `engine.h` and `sync_protocol.h` reference a non-existent `ioc_` member in comment docs

**Files modified:** `db/engine/engine.h`, `db/sync/sync_protocol.h`, `db/storage/storage.h`
**Commit:** e44d4d67
**Applied fix:** Rewrote the crypto-offload post-back guidance in the class-level
doc comments of `BlobEngine`, `SyncProtocol`, and `Storage` to match the idiom
actually used in `engine.cpp`:

```
co_await asio::post(co_await asio::this_coro::executor,
                    asio::use_awaitable);
```

The old guidance referred to an `ioc_` member that none of these three classes
owns, which would mislead a new contributor hunting for the real post-back
pattern. Removed the stray `db/peer/peer_types.h:58` cross-reference from
`storage.h` (it applied to NodeMetrics counters, not Storage) and pointed all
three headers at `engine.cpp` as the canonical example instead.

### WR-02: `ThreadOwner::reset_for_test()` alias is public surface and outlives its stated purpose

**Files modified:** `db/storage/thread_check.h`, `db/tests/util/test_storage_thread_check.cpp`
**Commit:** 9165af6c
**Applied fix:** Deleted the `reset_for_test()` alias (lines 94-95 of
`thread_check.h`) per project memory `feedback_no_backward_compat`. The alias
was justified by a "backwards-compat with existing call sites" comment, but
the only caller is the unit test this phase introduces — there are no
pre-existing call sites to protect. Updated the single call site in
`db/tests/util/test_storage_thread_check.cpp` to use `reset()` directly,
renamed the corresponding `TEST_CASE("reset_for_test — ...")` to
`TEST_CASE("reset — ...")`, and fixed the stale doc comment on line 84 that
still named the alias. Verified with a grep that no `reset_for_test` token
remains under `db/`.

## Skipped Issues

None.

---

_Fixed: 2026-04-19_
_Fixer: Claude (gsd-code-fixer)_
_Iteration: 1_
