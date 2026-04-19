#pragma once

// Runtime assertion that Storage is touched from exactly one thread after
// the first call. Compiles to a no-op under NDEBUG. Matches the
// "strand-confined to io_context thread" pattern documented in
// db/peer/peer_types.h:58.
//
// Usage (inside Storage::Impl):
//   struct Impl {
//     chromatindb::storage::ThreadOwner thread_owner_;
//     ...
//   };
//   StoreResult Storage::store_blob(...) {
//     STORAGE_THREAD_CHECK();
//     ...
//   }
//
// The owner TID is captured lazily on the first call (NOT in the
// constructor). This matters because Storage is constructed during
// daemon startup before `ioc.run()` begins, from whatever thread runs
// main(), but the long-running owner is the io_context thread.
//
// D-07/D-08/D-09 in 121-CONTEXT.md:
//   - D-07: captures on first call, asserts identity thereafter
//   - D-08: macro form chosen (clean call-site grep target)
//   - D-09: purely internal enforcement; does not leak into Storage's
//           public API (no templated executors, no token params)

#include <atomic>
#include <cassert>
#include <thread>

namespace chromatindb::storage {

/// Thread-identity owner used by STORAGE_THREAD_CHECK(). Holds an atomic
/// std::thread::id; default-constructed id means "no owner yet".
class ThreadOwner {
public:
    /// Assertion entry point. Called at the top of every public Storage
    /// method via the STORAGE_THREAD_CHECK() macro. The `method` argument
    /// is used only for assertion-failure diagnostics.
    inline void check(const char* method) noexcept {
        const std::thread::id cur = std::this_thread::get_id();
        std::thread::id expected = owner_.load(std::memory_order_acquire);
        if (expected == std::thread::id{}) {
            // First call: install current tid as owner. If another thread
            // raced us here (it shouldn't — we're supposed to be single
            // threaded), the loser's `expected` is updated with the winner's
            // value and the assertion below catches the violation.
            std::thread::id desired = cur;
            if (owner_.compare_exchange_strong(expected, desired,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
                return;  // We installed ourselves as owner.
            }
            // Lost the race: `expected` now holds the winner's tid. Fall
            // through to the identity check below.
        }
        assert(expected == cur && "Storage accessed from a thread other "
                                  "than the one that first touched it — "
                                  "invariant violated");
        (void)method;
    }

    /// Test-only: non-asserting variant that returns true on match. Used
    /// by unit tests to exercise cross-thread detection without aborting.
    /// Also installs the current thread as owner on first call, matching
    /// the semantics of check().
    inline bool try_check(const char* method) noexcept {
        const std::thread::id cur = std::this_thread::get_id();
        std::thread::id expected = owner_.load(std::memory_order_acquire);
        if (expected == std::thread::id{}) {
            std::thread::id desired = cur;
            if (owner_.compare_exchange_strong(expected, desired,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
                (void)method;
                return true;
            }
        }
        (void)method;
        return expected == cur;
    }

    /// Reset the owner to the "no owner" sentinel. Used by:
    ///   - Unit tests, to exercise re-capture semantics without reconstruction.
    ///   - The Storage constructor, to forget the thread that ran the startup
    ///     `rebuild_quota_aggregates()` path so the eventual io_context thread
    ///     can install itself as owner on its first real call.
    void reset() noexcept {
        owner_.store(std::thread::id{}, std::memory_order_release);
    }

    /// Test-only alias kept for backwards-compat with existing call sites.
    void reset_for_test() noexcept { reset(); }

    /// Test-only: inspect the current owner (default-constructed id
    /// means no owner has been captured yet).
    std::thread::id owner_for_test() const noexcept {
        return owner_.load(std::memory_order_acquire);
    }

private:
    std::atomic<std::thread::id> owner_{std::thread::id{}};
};

}  // namespace chromatindb::storage

#if defined(NDEBUG)
#define STORAGE_THREAD_CHECK() ((void)0)
#else
#define STORAGE_THREAD_CHECK() (this->impl_->thread_owner_.check(__func__))
#endif
