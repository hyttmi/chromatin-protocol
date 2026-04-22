---
phase: 121-storage-concurrency-invariant
reviewed: 2026-04-19T00:00:00Z
depth: standard
files_reviewed: 10
files_reviewed_list:
  - db/storage/thread_check.h
  - db/storage/storage.h
  - db/storage/storage.cpp
  - db/engine/engine.h
  - db/engine/engine.cpp
  - db/peer/peer_manager.h
  - db/sync/sync_protocol.h
  - db/tests/util/test_storage_thread_check.cpp
  - db/tests/storage/test_storage_concurrency_tsan.cpp
  - db/CMakeLists.txt
findings:
  critical: 0
  warning: 2
  info: 6
  total: 8
status: issues_found
---

# Phase 121: Code Review Report

**Reviewed:** 2026-04-19
**Depth:** standard
**Files Reviewed:** 10
**Status:** issues_found

## Summary

The Phase 121 change set retroactively proves and enforces the "Storage is
touched only from the io_context thread" invariant. The implementation is
sound on the critical axes:

- `ThreadOwner`'s atomic lazy-capture pattern is correct; the CAS logic
  reloads `expected` on loss and falls through to the identity assert.
- Every public `Storage` member in `storage.cpp` except the move ctor /
  dtor / assignment is gated by `STORAGE_THREAD_CHECK()` — verified by
  cross-reading every `Storage::...` definition against the macro grep
  (31 hits across 31 public methods; const methods included).
- Every `co_await crypto::offload(...)` in `engine.cpp` is followed by a
  post-back to the coroutine's executor before the next `storage_` access.
  5 offload sites, 5 post-backs; no remaining off-thread Storage calls in
  the ingest or delete_blob pipelines.
- The TSAN driver test uses one-identity-per-coroutine to avoid dedup
  collisions masking a race, and it exercises the full `BlobEngine::ingest`
  path, which is the reachable entry point that previously held the
  off-thread windows.

Concerns worth fixing before declaring the invariant "documented": two
warnings are stale / inaccurate comment docs that reference a member
(`ioc_`) the class does not own, and the `reset_for_test()` alias
survives as public surface area now that production code calls the
real `reset()`. Six info items flag minor redundancies (triple-nested
thread checks through store_blob overloads, SKIP placement, test-path
categorisation, etc.). None block ship.

## Warnings

### WR-01: `engine.h` and `sync_protocol.h` reference a non-existent `ioc_` member in comment docs

**File:** `db/engine/engine.h:71-75`, `db/sync/sync_protocol.h:35-39`
**Issue:** Both class-level doc comments tell the reader that after a
crypto offload "the coroutine MUST post back to ioc_ before touching
Storage." Neither `BlobEngine` nor `SyncProtocol` has an `ioc_` member.
The actual post-back pattern used in `engine.cpp` (lines 196, 254, 278,
409, 426) is `co_await asio::post(co_await asio::this_coro::executor,
asio::use_awaitable)` — it resolves the executor from the coroutine
itself, not from a stored reference. A new contributor reading this
header will go looking for an `ioc_` field that isn't there and will
not discover the real idiom. `storage.h:110-113` has the same problem:
its canonical example shows `co_await asio::post(ioc_, asio::use_awaitable)`
but the only class in the reviewed set that owns a member named `ioc_` is
`PeerManager`.
**Fix:** Rewrite the post-back guidance to match the idiom used in
engine.cpp:

```cpp
/// Callers that offload work to a thread_pool (via db/crypto/thread_pool.h)
/// MUST post back to the coroutine's executor before touching Storage:
///
///   co_await asio::post(co_await asio::this_coro::executor,
///                       asio::use_awaitable);
///
/// See engine.cpp for the canonical example.
```

Apply the same edit to `engine.h`, `sync_protocol.h`, and `storage.h`.

### WR-02: `ThreadOwner::reset_for_test()` alias is public surface and outlives its stated purpose

**File:** `db/storage/thread_check.h:94-95`
**Issue:** The comment block on `reset()` (lines 85-92) already documents
that the Storage constructor itself calls `reset()` to forget the
startup thread. That removes the only reason for a separate
`reset_for_test()` alias — the name no longer matches the method's
only remaining caller class (Storage's production ctor). The doc claims
the alias is kept "for backwards-compat with existing call sites," but
this phase introduces the class; there are no pre-existing call sites
to be compatible with. The function is public (reachable from any
`ThreadOwner` holder — including test files that construct one directly),
compiles into release binaries, and its name lies about its scope.
Per the project's `feedback_no_backward_compat` note, the preferred
answer is to delete the alias now rather than accrete it.
**Fix:** Delete lines 94-95 and update the three test sites in
`test_storage_thread_check.cpp` (lines 109, 113 reset_for_test calls)
to call `reset()` directly. The semantics are identical; the rename
is trivial.

## Info

### IN-01: Triple-nested `STORAGE_THREAD_CHECK()` across `store_blob` overload chain

**File:** `db/storage/storage.cpp:346, 355, 365`
**Issue:** `Storage::store_blob(blob)` (1-arg) checks, then calls the
3-arg overload which checks again, which calls the 6-arg overload
which checks a third time. In debug this runs three atomic loads plus
three CAS-or-compare cycles per single logical call. No correctness
impact (the atomic is idempotent once owner is set), just noise. The
outer overloads are trivial forwarders; the check only needs to fire
once per user-visible call.
**Fix:** Drop `STORAGE_THREAD_CHECK()` from the 1-arg and 3-arg
forwarders (lines 346 and 355). Keep the check on the 6-arg implementor
at line 365. The thread identity is already established by the first
check at the forwarder layer; the inner check is redundant.

### IN-02: `SKIP` directive in `cross_thread_aborts` test fires after the test has already asserted

**File:** `db/tests/util/test_storage_thread_check.cpp:66-70`
**Issue:** The `#ifdef NDEBUG SKIP(...)` lives at the END of the test
case, after the `REQUIRE` assertions that exercise `try_check` across
threads. The intent seems to be "this test doesn't really cover
`STORAGE_THREAD_CHECK()` itself in release builds" — but by the time
SKIP runs, the test has already passed (or failed) on its REQUIREs.
Catch2 will log the skip reason but not restore any test-reporting
state. Either the test has something meaningful to say in NDEBUG (then
SKIP is wrong — the REQUIREs *did* run), or it doesn't (then SKIP should
be at the top of the case, before any REQUIRE, so the body is actually
skipped).
**Fix:** Move the `#ifdef NDEBUG SKIP(...)` block to the top of the
TEST_CASE body, before line 47 (the first `REQUIRE`). Alternatively,
drop the SKIP entirely — the test-under-NDEBUG behaviour is already
covered by `macro_expands_in_release_to_noop`.

### IN-03: `owner_for_test()` is public but annotated "Test-only"; compiles into release binaries

**File:** `db/storage/thread_check.h:97-101`
**Issue:** Doc comment labels the method "Test-only" but it is public,
has no `#ifdef` guard, and is emitted into every build flavor. In
practice nothing outside the unit test can reach `thread_owner_` (it's
a private member of `Storage::Impl`), but the annotation is misleading.
If the "test-only" contract matters (symbol-stripping, API surface,
signalling intent to future readers), either:
- drop the comment and treat it as a plain public accessor, or
- guard the method with `#ifndef NDEBUG` or a project-internal `CHROMATINDB_TESTING` define.
**Fix:** Simplest: change the doc to `/// Inspect the current owner...
(primarily used by unit tests; cheap — one atomic load).` Same for
`try_check`. If the guarded variant is preferred, wrap both in
`#ifndef NDEBUG`.

### IN-04: Debug-only `check()` path also returns `void`; production assertion failure loses `method` context beyond assert message

**File:** `db/storage/thread_check.h:42-63`
**Issue:** The `method` parameter is threaded in via `__func__` by the
macro, but inside `check()` it's only consumed by the `assert()` string
"Storage accessed from a thread other than the one that first touched
it — invariant violated" — the macro-provided `method` name is captured
by the `(void)method;` line and never emitted. When the assertion
fires, the user sees a generic message and must run under a debugger
or consult `__func__` in the stack trace. A cheap improvement is to
log the method name too so crash triage doesn't require a debugger.
**Fix:** Replace the unused `(void)method` with either a `spdlog::error`
(logging infra is already pulled in by Storage) or a richer assert
message, e.g.:

```cpp
if (expected != cur) {
    std::fprintf(stderr,
        "STORAGE_THREAD_CHECK violation in %s (owner=%zu, current=%zu)\n",
        method,
        std::hash<std::thread::id>{}(expected),
        std::hash<std::thread::id>{}(cur));
    assert(false && "Storage accessed from a thread other than owner");
}
```

### IN-05: Test placed under `db/tests/util/` even though the SUT is a storage header

**File:** `db/CMakeLists.txt:251`, `db/tests/util/test_storage_thread_check.cpp`
**Issue:** `thread_check.h` lives in `db/storage/` and is namespaced
`chromatindb::storage::`. The test, however, lives in `db/tests/util/`.
This splits storage tests across two directories (`tests/storage/` for
blob/cursor tests, `tests/util/` for the thread-check) with no obvious
rule. A future reader auditing "what covers the storage concurrency
invariant" will find the TSAN driver under `tests/storage/` but not
the unit test.
**Fix:** Move `tests/util/test_storage_thread_check.cpp` to
`tests/storage/test_thread_check.cpp` and update the corresponding line
in `db/CMakeLists.txt`.

### IN-06: `Impl::thread_owner_` carries 8-16 bytes even in release builds where the macro is a no-op

**File:** `db/storage/storage.cpp:133-136`, `db/storage/thread_check.h:109-113`
**Issue:** `STORAGE_THREAD_CHECK()` compiles to `((void)0)` under
NDEBUG, but the `ThreadOwner` member inside `Storage::Impl` is
unconditional. Release builds carry an unused `std::atomic<std::thread::id>`
(implementation-defined size, typically 8 bytes) plus the alignment
footprint it adds to `Impl`. This is a trivial cost — Storage is
singleton-ish in production — but it's trivially avoidable by
`#ifndef NDEBUG`-guarding both the member and the declaration in
thread_check.h. Flagging as Info because (a) the overhead is negligible
per live Storage instance and (b) keeping the member present simplifies
any future swap to a release-build diagnostic (e.g., one-shot log on
mismatch).
**Fix:** Optional — either accept the overhead and document it in
thread_check.h, or guard `Impl::thread_owner_` with `#ifndef NDEBUG`
and update the macro so the NDEBUG path is both the member *and* the
call.

---

_Reviewed: 2026-04-19_
_Reviewer: Claude (gsd-code-reviewer)_
_Depth: standard_
