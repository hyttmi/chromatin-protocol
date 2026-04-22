// D-12(f) — cross-namespace PUBK race regression test.
//
// Anchor: VALIDATION.md row
//   phase122/tsan: cross-namespace PUBK race first-wins
//
// Pitfall #7 (Idempotent PUBK registration race): N concurrent coroutines
// all attempt to ingest the SAME PUBK for the SAME namespace. Because the
// bytes are bit-identical, every call to Storage::register_owner_pubkey
// sees either (a) no entry yet (inserts) or (b) an existing entry whose
// bytes match exactly (silent no-op). None of them throw. The main-DBI
// blob path dedups on content_hash, so only one stored blob exists.
//
// Expected outcome:
//   - All N ingests accepted.
//   - owner_pubkeys has exactly one entry for the shared namespace.
//   - No TSAN data-race warnings when built with -DSANITIZER=tsan.
//
// Tag: [phase122][tsan][pubk_first]. The file lives alongside the existing
// test_storage_concurrency_tsan.cpp and mirrors its fixture pattern — the
// test is TSAN-agnostic at source level; run under a sanitized build for
// the data-race check. Default debug build proves the concurrency driver
// runs correctly and the ingest post-conditions hold.

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <asio.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/thread_pool.hpp>
#include <asio/use_awaitable.hpp>

#include "db/engine/engine.h"
#include "db/identity/identity.h"
#include "db/storage/storage.h"
#include "db/wire/codec.h"

#include "db/tests/test_helpers.h"

using chromatindb::engine::BlobEngine;
using chromatindb::identity::NodeIdentity;
using chromatindb::storage::Storage;
using chromatindb::test::make_pubk_blob;
using chromatindb::test::TempDir;

// D-12(f): N concurrent PUBK ingests for the same namespace — all accept
// idempotently (bit-identical PUBK body -> register_owner_pubkey silent
// match after the first one wins, content_hash dedup at Step 2.5 for the
// main-DBI blob). Exactly one owner_pubkeys entry remains.
TEST_CASE("concurrent PUBK ingests for same namespace — all accept idempotently",
          "[phase122][tsan][pubk_first]") {
    TempDir tmp;
    Storage storage(tmp.path.string());
    asio::thread_pool pool(4);
    BlobEngine engine(storage, pool);

    auto id = NodeIdentity::generate();
    auto pubk = make_pubk_blob(id);
    auto target_ns_arr = id.namespace_id();
    std::span<const uint8_t, 32> target_ns(target_ns_arr);

    constexpr int kCoroutines = 4;

    asio::io_context ioc;
    std::atomic<int> accepted{0};
    std::atomic<int> completed{0};

    for (int i = 0; i < kCoroutines; ++i) {
        asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
            auto r = co_await engine.ingest(target_ns, pubk);
            if (r.accepted) {
                accepted.fetch_add(1, std::memory_order_relaxed);
            }
            completed.fetch_add(1, std::memory_order_relaxed);
            co_return;
        }, asio::detached);
    }

    ioc.run();
    pool.join();

    REQUIRE(completed.load() == kCoroutines);
    // Every ingest accepts: first wins the Step 4.5 register call, the rest
    // either hit owner_pubkeys with matching bytes (no-op) or dedup at the
    // content-hash layer (Step 2.5 short-circuit).
    REQUIRE(accepted.load() == kCoroutines);
    REQUIRE(storage.has_owner_pubkey(target_ns));
    REQUIRE(storage.count_owner_pubkeys() == 1);
}
