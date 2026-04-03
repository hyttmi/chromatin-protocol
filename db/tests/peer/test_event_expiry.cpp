#include <catch2/catch_test_macros.hpp>
#include <chrono>

#include "db/acl/access_control.h"
#include "db/peer/peer_manager.h"
#include "db/config/config.h"
#include "db/engine/engine.h"
#include "db/identity/identity.h"
#include "db/storage/storage.h"
#include "db/wire/codec.h"

#include "db/tests/test_helpers.h"

#include <asio.hpp>

using chromatindb::test::TempDir;
using chromatindb::test::make_signed_blob;
using chromatindb::test::current_timestamp;

using chromatindb::acl::AccessControl;
using chromatindb::config::Config;
using chromatindb::engine::BlobEngine;
using chromatindb::identity::NodeIdentity;
using chromatindb::peer::PeerManager;
using chromatindb::storage::Storage;

// ============================================================================
// Event-driven expiry tests (Phase 81 -- MAINT-01, MAINT-02, MAINT-03)
// ============================================================================

TEST_CASE("timer fires at exact expiry", "[event-expiry]") {
    TempDir tmp;

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();

    auto id = NodeIdentity::load_or_generate(tmp.path);

    // Injectable clock for storage: starts at real time
    uint64_t fake_time = current_timestamp();
    Storage store(tmp.path.string(), [&]() -> uint64_t { return fake_time; });

    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, id.namespace_id());

    // Store a blob with 2s TTL (expiry at fake_time + 2)
    auto blob = make_signed_blob(id, "expiry-test-1", 2, fake_time);
    {
        asio::io_context tmp_ioc;
        auto result = chromatindb::test::run_async(pool, eng.ingest(blob, nullptr));
        REQUIRE(result.accepted);
    }

    // Verify blob is stored and expiry is set
    auto earliest = store.get_earliest_expiry();
    REQUIRE(earliest.has_value());
    REQUIRE(*earliest == fake_time + 2);

    PeerManager pm(cfg, id, eng, store, ioc, pool, acl);
    pm.start();

    // Advance fake clock past expiry so run_expiry_scan() will purge
    fake_time += 3;

    // Run io_context long enough for the ~2s timer to fire
    ioc.run_for(std::chrono::seconds(4));

    // Blob should be purged
    auto after_expiry = store.get_earliest_expiry();
    REQUIRE_FALSE(after_expiry.has_value());

    pm.stop();
    ioc.run_for(std::chrono::milliseconds(100));
}

TEST_CASE("chain rearm after scan", "[event-expiry]") {
    TempDir tmp;

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();

    auto id = NodeIdentity::load_or_generate(tmp.path);

    uint64_t fake_time = current_timestamp();
    Storage store(tmp.path.string(), [&]() -> uint64_t { return fake_time; });

    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, id.namespace_id());

    // Store two blobs with staggered expiry: 2s and 4s
    auto blob1 = make_signed_blob(id, "chain-test-1", 2, fake_time);
    auto blob2 = make_signed_blob(id, "chain-test-2", 4, fake_time);
    {
        auto r1 = chromatindb::test::run_async(pool, eng.ingest(blob1, nullptr));
        REQUIRE(r1.accepted);
        auto r2 = chromatindb::test::run_async(pool, eng.ingest(blob2, nullptr));
        REQUIRE(r2.accepted);
    }

    auto earliest = store.get_earliest_expiry();
    REQUIRE(earliest.has_value());
    REQUIRE(*earliest == fake_time + 2);

    PeerManager pm(cfg, id, eng, store, ioc, pool, acl);
    pm.start();

    // Advance fake clock past first expiry
    fake_time += 3;
    // Run enough for first timer (~2s)
    ioc.run_for(std::chrono::seconds(4));

    // First blob purged, second still exists
    auto mid_expiry = store.get_earliest_expiry();
    REQUIRE(mid_expiry.has_value());
    REQUIRE(*mid_expiry == fake_time - 3 + 4);  // original fake_time + 4

    // Advance fake clock past second expiry
    fake_time += 2;  // Now at original + 5, past the 4s expiry
    // Run enough for second timer (~2s from first fire)
    ioc.run_for(std::chrono::seconds(4));

    // Both blobs purged
    auto final_expiry = store.get_earliest_expiry();
    REQUIRE_FALSE(final_expiry.has_value());

    pm.stop();
    ioc.run_for(std::chrono::milliseconds(100));
}

TEST_CASE("ingest rearm with shorter TTL", "[event-expiry]") {
    TempDir tmp;

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();

    auto id = NodeIdentity::load_or_generate(tmp.path);

    uint64_t fake_time = current_timestamp();
    Storage store(tmp.path.string(), [&]() -> uint64_t { return fake_time; });

    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, id.namespace_id());

    // Store a far-future blob (600s TTL)
    auto far_blob = make_signed_blob(id, "far-future", 600, fake_time);
    {
        auto r = chromatindb::test::run_async(pool, eng.ingest(far_blob, nullptr));
        REQUIRE(r.accepted);
    }

    PeerManager pm(cfg, id, eng, store, ioc, pool, acl);
    pm.start();

    // Let startup complete
    ioc.run_for(std::chrono::milliseconds(100));

    // Now ingest a short-TTL blob (2s) via on_blob_ingested (simulating peer replication)
    auto short_blob = make_signed_blob(id, "short-lived", 2, fake_time);
    {
        auto r = chromatindb::test::run_async(pool, eng.ingest(short_blob, nullptr));
        REQUIRE(r.accepted);
    }

    // Compute blob_hash for the short blob
    auto short_hash = chromatindb::crypto::sha3_256(short_blob.data);

    // Trigger the rearm by calling on_blob_ingested with the short expiry
    uint64_t short_expiry = fake_time + 2;
    pm.on_blob_ingested(
        short_blob.namespace_id, short_hash, 2,
        static_cast<uint32_t>(short_blob.data.size()),
        false, short_expiry, nullptr);

    // Advance fake clock past short-TTL expiry
    fake_time += 3;

    // Run long enough for the rearmed timer (~2s)
    ioc.run_for(std::chrono::seconds(4));

    // Short-TTL blob should be purged, far-future blob should remain
    auto remaining = store.get_earliest_expiry();
    REQUIRE(remaining.has_value());
    // The far-future blob expires at original fake_time + 600
    REQUIRE(*remaining == fake_time - 3 + 600);

    pm.stop();
    ioc.run_for(std::chrono::milliseconds(100));
}

TEST_CASE("no timer when storage empty", "[event-expiry]") {
    TempDir tmp;

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();

    auto id = NodeIdentity::load_or_generate(tmp.path);
    Storage store(tmp.path.string());

    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, id.namespace_id());

    PeerManager pm(cfg, id, eng, store, ioc, pool, acl);
    pm.start();

    // Run briefly
    ioc.run_for(std::chrono::milliseconds(200));

    // No expiring blobs, no timer
    auto earliest = store.get_earliest_expiry();
    REQUIRE_FALSE(earliest.has_value());

    pm.stop();
    ioc.run_for(std::chrono::milliseconds(100));
}

TEST_CASE("no timer for TTL=0 only blobs", "[event-expiry]") {
    TempDir tmp;

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();

    auto id = NodeIdentity::load_or_generate(tmp.path);
    Storage store(tmp.path.string());

    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, id.namespace_id());

    // Store a permanent blob (TTL=0)
    auto perm_blob = make_signed_blob(id, "permanent-blob", 0);
    {
        auto r = chromatindb::test::run_async(pool, eng.ingest(perm_blob, nullptr));
        REQUIRE(r.accepted);
    }

    PeerManager pm(cfg, id, eng, store, ioc, pool, acl);
    pm.start();

    // Run briefly
    ioc.run_for(std::chrono::milliseconds(200));

    // No expiring blobs (TTL=0 means permanent)
    auto earliest = store.get_earliest_expiry();
    REQUIRE_FALSE(earliest.has_value());

    pm.stop();
    ioc.run_for(std::chrono::milliseconds(100));
}
