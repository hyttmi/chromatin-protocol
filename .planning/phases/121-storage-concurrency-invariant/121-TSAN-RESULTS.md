# Phase 121: TSAN Results

## Build

- Flavor: Debug + -fsanitize=thread (via `-DSANITIZER=tsan`)
- Compiler: g++ (GCC) 15.2.1 20260209
- Suppressions: `sanitizers/tsan.supp` (3rd-party only; unchanged in this phase)
- Build directory: `build-tsan/`

Build command used:

```
cmake -S . -B build-tsan -DSANITIZER=tsan -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tsan -j$(nproc) --target chromatindb_tests
```

Build warnings observed: several `warning: ‘atomic_thread_fence’ is not supported
with ‘-fsanitize=thread’ [-Wtsan]` inside Asio's `std_fenced_block.hpp`. These are
a known upstream Asio limitation and pre-date this phase; they do not indicate a
race in chromatindb code. No `sanitizers/tsan.supp` changes needed.

## Concurrency Test

- Test: `[tsan][storage][concurrency]`
- Test file: `db/tests/storage/test_storage_concurrency_tsan.cpp`
- Drives: 8 coroutines × 16 ingests = 128 concurrent ingests via `BlobEngine::ingest`
  through a 4-worker `asio::thread_pool` with `crypto::offload`
- Result: **PASS**
- Assertions: 11 / 11 passed (wrapping 128 ingests, 8 namespace assertions, 2 global counters)
- TSAN warnings: **0**

Command:
```
TSAN_OPTIONS="suppressions=$(pwd)/sanitizers/tsan.supp halt_on_error=1 second_deadlock_stack=1" \
  ./build-tsan/db/chromatindb_tests "[tsan][storage][concurrency]" --reporter compact
```

Output:
```
Filters: [tsan] [storage] [concurrency]
RNG seed: 3487049872
[info] Generated master key at /tmp/chromatindb_test_*/master.key
[info] Storage opened at /tmp/chromatindb_test_* with encryption at rest
[info] Quota rebuild: 0 namespaces, 0 total blobs
All tests passed (11 assertions in 1 test case)
```

## Regression Suite

Ran `./build-tsan/db/chromatindb_tests "[storage],[engine],[sync]"` under TSAN:

| Tag       | Test Cases | Assertions | PASS | TSAN warnings |
|-----------|-----------:|-----------:|------|--------------:|
| aggregate |        233 |       1473 | yes  |             0 |

Raw output footer:
```
All tests passed (1473 assertions in 233 test cases)
```

Command:
```
TSAN_OPTIONS="suppressions=$(pwd)/sanitizers/tsan.supp halt_on_error=1" \
  ./build-tsan/db/chromatindb_tests "[storage],[engine],[sync]" --reporter compact
```

Note: the `[storage]` tag includes the new concurrency test, so it counts into the
same 233-test aggregate. Running each tag alone is not necessary for the ship gate;
what matters is that every test across these three tags exits clean.

## Conditional Fix Applied

The audit (121-AUDIT.md) identified 9 executor-C call sites in `db/engine/engine.cpp`
where Storage was accessed on thread_pool worker threads without a post-back to
the io_context. Fix landed in the same commit as Task 3 (subject:
`feat(121-01): apply STORAGE_THREAD_CHECK + fix ingest post-back (Task 3+6)`):

- After each of 4 `co_await crypto::offload(pool_, ...)` sites in `BlobEngine::ingest`
  and `BlobEngine::delete_blob`, added:
  ```cpp
  co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
  ```
- Mechanism: post-back to the caller's executor (no new strand). Matches the
  uniform idiom already used in `message_dispatcher.cpp:339/1351`,
  `blob_push_manager.cpp:194`, `pex_manager.cpp:358`,
  `connection_manager.cpp:300`, `sync_orchestrator.cpp:94`.
- Rationale: strand confinement (D-06 "preferred, but not locked") would add a
  new concurrency primitive where a perfectly working idiom already exists in
  the codebase. Every caller of `engine.ingest` already runs on ioc_. The fix
  is additive, minimal, and makes engine.cpp consistent with the rest of `db/peer/`.

Storage comments now say **"thread-confined"** (not "strand-confined") in
storage.h, engine.h, peer_manager.h, sync_protocol.h.

## Ship Gate

- Verdict: **INVARIANT PROVEN**
- Evidence:
  1. `[tsan][storage][concurrency]` passes under TSAN with 0 warnings (above).
  2. `[storage],[engine],[sync]` regression suite is TSAN-clean with 1473 assertions.
  3. `STORAGE_THREAD_CHECK()` is live at all 32 public Storage method entry points
     and never aborts under the full debug test run (232+ tests) nor under TSAN.
  4. `sanitizers/tsan.supp` is unchanged versus the phase start (no new suppressions):
     ```
     $ git diff --stat d5cfd77a..HEAD -- sanitizers/tsan.supp
     (empty — file untouched)
     ```
