#include <catch2/catch_test_macros.hpp>

#include "db/peer/blob_push_manager.h"
#include "db/peer/peer_types.h"

#include <array>
#include <cstring>
#include <unordered_map>

using chromatindb::peer::ArrayHash64;
using chromatindb::peer::ArrayHash32;
using chromatindb::peer::make_pending_key;

// ============================================================================
// make_pending_key tests (SYNC-02: composite namespace||hash key)
// ============================================================================

TEST_CASE("make_pending_key produces 64-byte concatenation", "[sync][correctness]") {
    std::array<uint8_t, 32> ns{};
    std::array<uint8_t, 32> hash{};

    // Fill with distinct patterns
    std::memset(ns.data(), 0xAA, 32);
    std::memset(hash.data(), 0xBB, 32);

    auto key = make_pending_key(ns, hash);

    // First 32 bytes = namespace
    for (size_t i = 0; i < 32; ++i) {
        REQUIRE(key[i] == 0xAA);
    }
    // Last 32 bytes = hash
    for (size_t i = 32; i < 64; ++i) {
        REQUIRE(key[i] == 0xBB);
    }
}

TEST_CASE("make_pending_key: same ns+hash always produces same key", "[sync][correctness]") {
    std::array<uint8_t, 32> ns{};
    std::array<uint8_t, 32> hash{};
    ns[0] = 0x01;
    hash[0] = 0x02;

    auto key1 = make_pending_key(ns, hash);
    auto key2 = make_pending_key(ns, hash);

    REQUIRE(key1 == key2);
}

TEST_CASE("make_pending_key: different namespaces with same hash produce different keys (SYNC-02)", "[sync][correctness]") {
    // This is the core SYNC-02 fix: two blobs with the same hash but different
    // namespaces must NOT collide in pending_fetches_.
    std::array<uint8_t, 32> ns_a{};
    std::array<uint8_t, 32> ns_b{};
    std::array<uint8_t, 32> hash{};

    ns_a[0] = 0x01;
    ns_b[0] = 0x02;
    std::memset(hash.data(), 0xFF, 32);

    auto key_a = make_pending_key(ns_a, hash);
    auto key_b = make_pending_key(ns_b, hash);

    REQUIRE(key_a != key_b);
}

TEST_CASE("make_pending_key: different hashes with same namespace produce different keys", "[sync][correctness]") {
    std::array<uint8_t, 32> ns{};
    std::array<uint8_t, 32> hash_a{};
    std::array<uint8_t, 32> hash_b{};

    std::memset(ns.data(), 0xCC, 32);
    hash_a[0] = 0x01;
    hash_b[0] = 0x02;

    auto key_a = make_pending_key(ns, hash_a);
    auto key_b = make_pending_key(ns, hash_b);

    REQUIRE(key_a != key_b);
}

// ============================================================================
// ArrayHash64 functor tests
// ============================================================================

TEST_CASE("ArrayHash64 produces consistent hashes", "[sync][correctness]") {
    ArrayHash64 hasher;

    std::array<uint8_t, 64> key{};
    key[0] = 0x42;
    key[7] = 0xFF;

    auto h1 = hasher(key);
    auto h2 = hasher(key);

    REQUIRE(h1 == h2);
}

TEST_CASE("ArrayHash64: different keys produce different hashes", "[sync][correctness]") {
    ArrayHash64 hasher;

    std::array<uint8_t, 64> key_a{};
    std::array<uint8_t, 64> key_b{};

    // Differ in the first 8 bytes (which the hasher reads)
    key_a[0] = 0x01;
    key_b[0] = 0x02;

    REQUIRE(hasher(key_a) != hasher(key_b));
}

// ============================================================================
// Composite key in unordered_map (SYNC-02 integration)
// ============================================================================

TEST_CASE("composite keys work correctly in unordered_map (SYNC-01/SYNC-02)", "[sync][correctness]") {
    // Simulate pending_fetches_ behavior with the new 64-byte key type
    std::unordered_map<std::array<uint8_t, 64>, int, ArrayHash64> pending;

    std::array<uint8_t, 32> ns_a{};
    std::array<uint8_t, 32> ns_b{};
    std::array<uint8_t, 32> hash{};

    ns_a[0] = 0x01;
    ns_b[0] = 0x02;
    std::memset(hash.data(), 0xAB, 32);

    auto key_a = make_pending_key(ns_a, hash);
    auto key_b = make_pending_key(ns_b, hash);

    // Insert both -- should not collide (SYNC-02)
    pending.emplace(key_a, 1);
    pending.emplace(key_b, 2);
    REQUIRE(pending.size() == 2);

    // Lookup by exact key
    REQUIRE(pending.count(key_a) == 1);
    REQUIRE(pending.count(key_b) == 1);
    REQUIRE(pending[key_a] == 1);
    REQUIRE(pending[key_b] == 2);

    // Erase one -- other remains (SYNC-01: unconditional cleanup)
    pending.erase(key_a);
    REQUIRE(pending.size() == 1);
    REQUIRE(pending.count(key_a) == 0);
    REQUIRE(pending.count(key_b) == 1);

    // Re-insert after erase (simulates re-fetch after cleanup)
    pending.emplace(key_a, 3);
    REQUIRE(pending.size() == 2);
    REQUIRE(pending[key_a] == 3);
}

TEST_CASE("clean_pending_fetches pattern works with 64-byte keys", "[sync][correctness]") {
    // Simulate the clean_pending_fetches iterator pattern with new key type
    std::unordered_map<std::array<uint8_t, 64>, int, ArrayHash64> pending;

    std::array<uint8_t, 32> ns{};
    std::array<uint8_t, 32> hash_a{};
    std::array<uint8_t, 32> hash_b{};
    std::array<uint8_t, 32> hash_c{};

    hash_a[0] = 0x01;
    hash_b[0] = 0x02;
    hash_c[0] = 0x03;

    // conn_id 1 owns two entries, conn_id 2 owns one
    pending.emplace(make_pending_key(ns, hash_a), 1);  // conn 1
    pending.emplace(make_pending_key(ns, hash_b), 2);  // conn 2
    pending.emplace(make_pending_key(ns, hash_c), 1);  // conn 1

    REQUIRE(pending.size() == 3);

    // Simulate clean_pending_fetches for conn_id 1
    for (auto it = pending.begin(); it != pending.end(); ) {
        if (it->second == 1) {
            it = pending.erase(it);
        } else {
            ++it;
        }
    }

    REQUIRE(pending.size() == 1);
    REQUIRE(pending.count(make_pending_key(ns, hash_b)) == 1);
}
