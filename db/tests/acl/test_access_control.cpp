#include <catch2/catch_test_macros.hpp>
#include "db/acl/access_control.h"
#include "db/crypto/hash.h"
#include "db/identity/identity.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

using namespace chromatindb::acl;

namespace {

/// Convert 32 bytes to hex string for test setup.
std::string to_hex(std::span<const uint8_t, 32> bytes) {
    static constexpr char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (auto b : bytes) {
        result += hex_chars[(b >> 4) & 0xF];
        result += hex_chars[b & 0xF];
    }
    return result;
}

/// Generate a deterministic test namespace hash from a seed.
std::array<uint8_t, 32> make_ns_hash(uint8_t seed) {
    std::array<uint8_t, 32> data{};
    data.fill(seed);
    return chromatindb::crypto::sha3_256(data);
}

} // anonymous namespace

TEST_CASE("AccessControl with empty keys is open mode", "[access_control]") {
    auto own_ns = make_ns_hash(0);
    AccessControl acl({}, std::span<const uint8_t, 32>(own_ns));

    REQUIRE_FALSE(acl.is_closed_mode());
    REQUIRE(acl.allowed_count() == 0);
}

TEST_CASE("AccessControl with non-empty keys is closed mode", "[access_control]") {
    auto own_ns = make_ns_hash(0);
    auto other_ns = make_ns_hash(1);
    std::string other_hex = to_hex(std::span<const uint8_t, 32>(other_ns));

    AccessControl acl({other_hex}, std::span<const uint8_t, 32>(own_ns));

    REQUIRE(acl.is_closed_mode());
    REQUIRE(acl.allowed_count() == 1);
}

TEST_CASE("Open mode allows any namespace hash", "[access_control]") {
    auto own_ns = make_ns_hash(0);
    AccessControl acl({}, std::span<const uint8_t, 32>(own_ns));

    auto random_ns = make_ns_hash(42);
    REQUIRE(acl.is_allowed(std::span<const uint8_t, 32>(random_ns)));
}

TEST_CASE("Closed mode allows a namespace in the set", "[access_control]") {
    auto own_ns = make_ns_hash(0);
    auto allowed_ns = make_ns_hash(1);
    std::string allowed_hex = to_hex(std::span<const uint8_t, 32>(allowed_ns));

    AccessControl acl({allowed_hex}, std::span<const uint8_t, 32>(own_ns));

    REQUIRE(acl.is_allowed(std::span<const uint8_t, 32>(allowed_ns)));
}

TEST_CASE("Closed mode rejects a namespace NOT in the set", "[access_control]") {
    auto own_ns = make_ns_hash(0);
    auto allowed_ns = make_ns_hash(1);
    auto rejected_ns = make_ns_hash(2);
    std::string allowed_hex = to_hex(std::span<const uint8_t, 32>(allowed_ns));

    AccessControl acl({allowed_hex}, std::span<const uint8_t, 32>(own_ns));

    REQUIRE_FALSE(acl.is_allowed(std::span<const uint8_t, 32>(rejected_ns)));
}

TEST_CASE("Own namespace is always allowed even if not listed", "[access_control]") {
    auto own_ns = make_ns_hash(0);
    auto other_ns = make_ns_hash(1);
    std::string other_hex = to_hex(std::span<const uint8_t, 32>(other_ns));

    // own_ns is NOT in the hex_keys list, but should be implicitly allowed
    AccessControl acl({other_hex}, std::span<const uint8_t, 32>(own_ns));

    REQUIRE(acl.is_allowed(std::span<const uint8_t, 32>(own_ns)));
}

TEST_CASE("allowed_count returns configured key count", "[access_control]") {
    auto own_ns = make_ns_hash(0);
    auto ns1 = make_ns_hash(1);
    auto ns2 = make_ns_hash(2);
    std::string hex1 = to_hex(std::span<const uint8_t, 32>(ns1));
    std::string hex2 = to_hex(std::span<const uint8_t, 32>(ns2));

    AccessControl acl({hex1, hex2}, std::span<const uint8_t, 32>(own_ns));

    // Should be 2 (the configured keys), not 3 (which would include implicit self)
    REQUIRE(acl.allowed_count() == 2);
}

TEST_CASE("reload updates the set and returns correct diff", "[access_control]") {
    auto own_ns = make_ns_hash(0);
    auto ns1 = make_ns_hash(1);
    auto ns2 = make_ns_hash(2);
    auto ns3 = make_ns_hash(3);
    std::string hex1 = to_hex(std::span<const uint8_t, 32>(ns1));
    std::string hex2 = to_hex(std::span<const uint8_t, 32>(ns2));
    std::string hex3 = to_hex(std::span<const uint8_t, 32>(ns3));

    AccessControl acl({hex1, hex2}, std::span<const uint8_t, 32>(own_ns));

    // Reload: remove ns1, keep ns2, add ns3
    auto result = acl.reload({hex2, hex3});

    REQUIRE(result.added == 1);    // ns3
    REQUIRE(result.removed == 1);  // ns1
    REQUIRE(acl.allowed_count() == 2);
}

TEST_CASE("reload with empty keys switches to open mode", "[access_control]") {
    auto own_ns = make_ns_hash(0);
    auto ns1 = make_ns_hash(1);
    std::string hex1 = to_hex(std::span<const uint8_t, 32>(ns1));

    AccessControl acl({hex1}, std::span<const uint8_t, 32>(own_ns));
    REQUIRE(acl.is_closed_mode());

    auto result = acl.reload({});
    REQUIRE_FALSE(acl.is_closed_mode());
    REQUIRE(result.removed == 1);
    REQUIRE(result.added == 0);
}

TEST_CASE("reload from open to closed mode", "[access_control]") {
    auto own_ns = make_ns_hash(0);
    AccessControl acl({}, std::span<const uint8_t, 32>(own_ns));
    REQUIRE_FALSE(acl.is_closed_mode());

    auto ns1 = make_ns_hash(1);
    std::string hex1 = to_hex(std::span<const uint8_t, 32>(ns1));

    auto result = acl.reload({hex1});
    REQUIRE(acl.is_closed_mode());
    REQUIRE(result.added == 1);
    REQUIRE(result.removed == 0);
}

TEST_CASE("is_allowed with namespace derived from SHA3-256 of pubkey", "[access_control]") {
    // Generate a real identity to get a real pubkey -> namespace hash
    auto identity = chromatindb::identity::NodeIdentity::generate();
    auto own_ns_span = identity.namespace_id();  // span<const uint8_t, 32>

    // Generate another identity to test against
    auto peer = chromatindb::identity::NodeIdentity::generate();
    auto peer_ns = chromatindb::crypto::sha3_256(peer.public_key());
    std::string peer_hex = to_hex(std::span<const uint8_t, 32>(peer_ns));

    AccessControl acl({peer_hex}, own_ns_span);

    // Peer should be allowed
    REQUIRE(acl.is_allowed(std::span<const uint8_t, 32>(peer_ns)));

    // A random third party should NOT be allowed
    auto third = chromatindb::identity::NodeIdentity::generate();
    auto third_ns = chromatindb::crypto::sha3_256(third.public_key());
    REQUIRE_FALSE(acl.is_allowed(std::span<const uint8_t, 32>(third_ns)));
}

TEST_CASE("reload correctly changes is_allowed results", "[access_control]") {
    auto own_ns = make_ns_hash(0);
    auto ns1 = make_ns_hash(1);
    auto ns2 = make_ns_hash(2);
    std::string hex1 = to_hex(std::span<const uint8_t, 32>(ns1));
    std::string hex2 = to_hex(std::span<const uint8_t, 32>(ns2));

    AccessControl acl({hex1}, std::span<const uint8_t, 32>(own_ns));

    REQUIRE(acl.is_allowed(std::span<const uint8_t, 32>(ns1)));
    REQUIRE_FALSE(acl.is_allowed(std::span<const uint8_t, 32>(ns2)));

    // Reload: remove ns1, add ns2
    acl.reload({hex2});

    REQUIRE_FALSE(acl.is_allowed(std::span<const uint8_t, 32>(ns1)));
    REQUIRE(acl.is_allowed(std::span<const uint8_t, 32>(ns2)));
}
