// ship gate: concurrent-ingest driver that exercises the
// real BlobEngine::ingest path across multiple coroutines and verifies
// Storage remains consistent + STORAGE_THREAD_CHECK never fires.
//
// The test is TSAN-agnostic — it's just a concurrency driver. Run it
// under a `-DSANITIZER=tsan` build (Task 6) for the actual race check.
// In the default debug build it proves the code at least runs and
// produces correct per-namespace blob counts.

#include <array>
#include <atomic>
#include <memory>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <asio.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/thread_pool.hpp>
#include <asio/use_awaitable.hpp>

#include "db/engine/engine.h"
#include "db/identity/identity.h"
#include "db/storage/storage.h"
#include "db/tests/test_helpers.h"
#include "db/wire/codec.h"

using chromatindb::engine::BlobEngine;
using chromatindb::identity::NodeIdentity;
using chromatindb::storage::Storage;
using chromatindb::test::make_signed_blob;
using chromatindb::test::TempDir;

namespace {

// Build a unique signed blob in the given identity's namespace with a
// payload that makes content_hash distinct across (coroutine, iteration).
// Using unique identities-per-coroutine is cleaner than tampering with
// namespace_id bytes because the engine's namespace-ownership check is
// SHA3-256(pubkey) == namespace_id — the namespace is the hash of the
// signer's key. Each coroutine signs with its own identity.
chromatindb::wire::BlobData build_unique_blob(const NodeIdentity& id,
                                              int coroutine_index,
                                              int iteration) {
    // Payload embeds both indices so each (coroutine,iteration) is unique.
    std::string payload = "c=" + std::to_string(coroutine_index) +
                          ",i=" + std::to_string(iteration);
    return make_signed_blob(id, payload, /*ttl=*/604800);
}

}  // namespace

TEST_CASE("concurrent ingests are TSAN-clean and all succeed",
          "[tsan][storage][concurrency]") {
    TempDir tmp;
    Storage storage(tmp.path.string());
    asio::thread_pool pool(4);
    BlobEngine engine(storage, pool);

    constexpr int kCoroutines = 8;
    constexpr int kIngestsPerCoroutine = 16;

    // One identity per coroutine → one distinct namespace per coroutine.
    // Prevents dedup collisions from masking a race.
    std::vector<NodeIdentity> identities;
    identities.reserve(kCoroutines);
    for (int c = 0; c < kCoroutines; ++c) {
        identities.push_back(NodeIdentity::generate());
    }

    // PUBK-first: register owner PUBKs BEFORE the concurrent burst
    // so every non-PUBK write is allowed through the Step 1.5 gate.
    for (const auto& id : identities) {
        chromatindb::test::register_pubk(storage, id);
    }

    asio::io_context ioc;
    std::atomic<int> completed{0};
    std::atomic<int> accepted{0};

    for (int c = 0; c < kCoroutines; ++c) {
        asio::co_spawn(ioc, [&, c]() -> asio::awaitable<void> {
            for (int i = 0; i < kIngestsPerCoroutine; ++i) {
                auto blob = build_unique_blob(identities[c], c, i);
                auto result = co_await engine.ingest(
                    std::span<const uint8_t, 32>(identities[c].namespace_id()), blob);
                if (result.accepted) {
                    accepted.fetch_add(1, std::memory_order_relaxed);
                }
            }
            completed.fetch_add(1, std::memory_order_relaxed);
            co_return;
        }, asio::detached);
    }

    ioc.run();
    pool.join();

    REQUIRE(completed.load() == kCoroutines);
    REQUIRE(accepted.load() == kCoroutines * kIngestsPerCoroutine);

    auto namespaces = storage.list_namespaces();
    REQUIRE(namespaces.size() == static_cast<size_t>(kCoroutines));

    for (int c = 0; c < kCoroutines; ++c) {
        auto ns_span = identities[c].namespace_id();
        auto blobs = storage.get_blobs_by_seq(ns_span, 0);
        REQUIRE(blobs.size() == static_cast<size_t>(kIngestsPerCoroutine));
    }
}
