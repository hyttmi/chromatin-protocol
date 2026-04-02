#include <catch2/catch_test_macros.hpp>
#include "db/acl/access_control.h"
#include "db/crypto/hash.h"
#include "db/identity/identity.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "db/util/hex.h"

using namespace chromatindb::acl;

namespace {

using chromatindb::util::to_hex;

/// Generate a deterministic test namespace hash from a seed.
std::array<uint8_t, 32> make_ns_hash(uint8_t seed) {
    std::array<uint8_t, 32> data{};
    data.fill(seed);
    return chromatindb::crypto::sha3_256(data);
}

} // anonymous namespace

TEST_CASE("AccessControl with empty keys is open mode for both client and peer", "[access_control]") {
    auto own_ns = make_ns_hash(0);
    AccessControl acl({}, {}, std::span<const uint8_t, 32>(own_ns));

    REQUIRE_FALSE(acl.is_client_closed_mode());
    REQUIRE_FALSE(acl.is_peer_closed_mode());
    REQUIRE(acl.client_allowed_count() == 0);
    REQUIRE(acl.peer_allowed_count() == 0);
}

TEST_CASE("AccessControl with non-empty client keys is client-closed mode", "[access_control]") {
    auto own_ns = make_ns_hash(0);
    auto other_ns = make_ns_hash(1);
    std::string other_hex = to_hex(std::span<const uint8_t, 32>(other_ns));

    AccessControl acl({other_hex}, {}, std::span<const uint8_t, 32>(own_ns));

    REQUIRE(acl.is_client_closed_mode());
    REQUIRE_FALSE(acl.is_peer_closed_mode());
    REQUIRE(acl.client_allowed_count() == 1);
    REQUIRE(acl.peer_allowed_count() == 0);
}

TEST_CASE("AccessControl with non-empty peer keys is peer-closed mode", "[access_control]") {
    auto own_ns = make_ns_hash(0);
    auto other_ns = make_ns_hash(1);
    std::string other_hex = to_hex(std::span<const uint8_t, 32>(other_ns));

    AccessControl acl({}, {other_hex}, std::span<const uint8_t, 32>(own_ns));

    REQUIRE_FALSE(acl.is_client_closed_mode());
    REQUIRE(acl.is_peer_closed_mode());
    REQUIRE(acl.client_allowed_count() == 0);
    REQUIRE(acl.peer_allowed_count() == 1);
}

TEST_CASE("Open mode allows any namespace hash for both client and peer", "[access_control]") {
    auto own_ns = make_ns_hash(0);
    AccessControl acl({}, {}, std::span<const uint8_t, 32>(own_ns));

    auto random_ns = make_ns_hash(42);
    REQUIRE(acl.is_client_allowed(std::span<const uint8_t, 32>(random_ns)));
    REQUIRE(acl.is_peer_allowed(std::span<const uint8_t, 32>(random_ns)));
}

TEST_CASE("Client-closed mode allows client in the set", "[access_control]") {
    auto own_ns = make_ns_hash(0);
    auto allowed_ns = make_ns_hash(1);
    std::string allowed_hex = to_hex(std::span<const uint8_t, 32>(allowed_ns));

    AccessControl acl({allowed_hex}, {}, std::span<const uint8_t, 32>(own_ns));

    REQUIRE(acl.is_client_allowed(std::span<const uint8_t, 32>(allowed_ns)));
}

TEST_CASE("Client-closed mode rejects client NOT in the set", "[access_control]") {
    auto own_ns = make_ns_hash(0);
    auto allowed_ns = make_ns_hash(1);
    auto rejected_ns = make_ns_hash(2);
    std::string allowed_hex = to_hex(std::span<const uint8_t, 32>(allowed_ns));

    AccessControl acl({allowed_hex}, {}, std::span<const uint8_t, 32>(own_ns));

    REQUIRE_FALSE(acl.is_client_allowed(std::span<const uint8_t, 32>(rejected_ns)));
}

TEST_CASE("Peer-closed mode allows peer in the set", "[access_control]") {
    auto own_ns = make_ns_hash(0);
    auto allowed_ns = make_ns_hash(1);
    std::string allowed_hex = to_hex(std::span<const uint8_t, 32>(allowed_ns));

    AccessControl acl({}, {allowed_hex}, std::span<const uint8_t, 32>(own_ns));

    REQUIRE(acl.is_peer_allowed(std::span<const uint8_t, 32>(allowed_ns)));
}

TEST_CASE("Peer-closed mode rejects peer NOT in the set", "[access_control]") {
    auto own_ns = make_ns_hash(0);
    auto allowed_ns = make_ns_hash(1);
    auto rejected_ns = make_ns_hash(2);
    std::string allowed_hex = to_hex(std::span<const uint8_t, 32>(allowed_ns));

    AccessControl acl({}, {allowed_hex}, std::span<const uint8_t, 32>(own_ns));

    REQUIRE_FALSE(acl.is_peer_allowed(std::span<const uint8_t, 32>(rejected_ns)));
}

TEST_CASE("Client-closed + peer-open: unlisted key rejected as client, allowed as peer", "[access_control]") {
    auto own_ns = make_ns_hash(0);
    auto client_ns = make_ns_hash(1);
    auto unknown_ns = make_ns_hash(2);
    std::string client_hex = to_hex(std::span<const uint8_t, 32>(client_ns));

    AccessControl acl({client_hex}, {}, std::span<const uint8_t, 32>(own_ns));

    REQUIRE_FALSE(acl.is_client_allowed(std::span<const uint8_t, 32>(unknown_ns)));
    REQUIRE(acl.is_peer_allowed(std::span<const uint8_t, 32>(unknown_ns)));
}

TEST_CASE("Peer-closed + client-open: unlisted key allowed as client, rejected as peer", "[access_control]") {
    auto own_ns = make_ns_hash(0);
    auto peer_ns = make_ns_hash(1);
    auto unknown_ns = make_ns_hash(2);
    std::string peer_hex = to_hex(std::span<const uint8_t, 32>(peer_ns));

    AccessControl acl({}, {peer_hex}, std::span<const uint8_t, 32>(own_ns));

    REQUIRE(acl.is_client_allowed(std::span<const uint8_t, 32>(unknown_ns)));
    REQUIRE_FALSE(acl.is_peer_allowed(std::span<const uint8_t, 32>(unknown_ns)));
}

TEST_CASE("Both closed with different key sets", "[access_control]") {
    auto own_ns = make_ns_hash(0);
    auto client_only_ns = make_ns_hash(1);
    auto peer_only_ns = make_ns_hash(2);
    std::string client_hex = to_hex(std::span<const uint8_t, 32>(client_only_ns));
    std::string peer_hex = to_hex(std::span<const uint8_t, 32>(peer_only_ns));

    AccessControl acl({client_hex}, {peer_hex}, std::span<const uint8_t, 32>(own_ns));

    REQUIRE(acl.is_client_closed_mode());
    REQUIRE(acl.is_peer_closed_mode());

    // Client-only key: allowed as client, rejected as peer
    REQUIRE(acl.is_client_allowed(std::span<const uint8_t, 32>(client_only_ns)));
    REQUIRE_FALSE(acl.is_peer_allowed(std::span<const uint8_t, 32>(client_only_ns)));

    // Peer-only key: rejected as client, allowed as peer
    REQUIRE_FALSE(acl.is_client_allowed(std::span<const uint8_t, 32>(peer_only_ns)));
    REQUIRE(acl.is_peer_allowed(std::span<const uint8_t, 32>(peer_only_ns)));
}

TEST_CASE("Own namespace is always allowed in both lists when closed", "[access_control]") {
    auto own_ns = make_ns_hash(0);
    auto other_ns = make_ns_hash(1);
    std::string other_hex = to_hex(std::span<const uint8_t, 32>(other_ns));

    // own_ns is NOT in either hex_keys list, but should be implicitly allowed in both
    AccessControl acl({other_hex}, {other_hex}, std::span<const uint8_t, 32>(own_ns));

    REQUIRE(acl.is_client_allowed(std::span<const uint8_t, 32>(own_ns)));
    REQUIRE(acl.is_peer_allowed(std::span<const uint8_t, 32>(own_ns)));
}

TEST_CASE("client_allowed_count and peer_allowed_count are independent", "[access_control]") {
    auto own_ns = make_ns_hash(0);
    auto ns1 = make_ns_hash(1);
    auto ns2 = make_ns_hash(2);
    auto ns3 = make_ns_hash(3);
    std::string hex1 = to_hex(std::span<const uint8_t, 32>(ns1));
    std::string hex2 = to_hex(std::span<const uint8_t, 32>(ns2));
    std::string hex3 = to_hex(std::span<const uint8_t, 32>(ns3));

    // 2 client keys, 1 peer key
    AccessControl acl({hex1, hex2}, {hex3}, std::span<const uint8_t, 32>(own_ns));

    REQUIRE(acl.client_allowed_count() == 2);
    REQUIRE(acl.peer_allowed_count() == 1);
}

TEST_CASE("reload updates both sets and returns correct diff", "[access_control]") {
    auto own_ns = make_ns_hash(0);
    auto ns1 = make_ns_hash(1);
    auto ns2 = make_ns_hash(2);
    auto ns3 = make_ns_hash(3);
    auto ns4 = make_ns_hash(4);
    std::string hex1 = to_hex(std::span<const uint8_t, 32>(ns1));
    std::string hex2 = to_hex(std::span<const uint8_t, 32>(ns2));
    std::string hex3 = to_hex(std::span<const uint8_t, 32>(ns3));
    std::string hex4 = to_hex(std::span<const uint8_t, 32>(ns4));

    AccessControl acl({hex1, hex2}, {hex3}, std::span<const uint8_t, 32>(own_ns));

    // Reload: client remove ns1 keep ns2 add ns4, peer remove ns3
    auto result = acl.reload({hex2, hex4}, {});

    REQUIRE(result.client_added == 1);    // ns4
    REQUIRE(result.client_removed == 1);  // ns1
    REQUIRE(result.peer_added == 0);
    REQUIRE(result.peer_removed == 1);    // ns3
    REQUIRE(acl.client_allowed_count() == 2);
    REQUIRE(acl.peer_allowed_count() == 0);
}

TEST_CASE("reload changes only client keys -- peer keys unchanged", "[access_control]") {
    auto own_ns = make_ns_hash(0);
    auto ns1 = make_ns_hash(1);
    auto ns2 = make_ns_hash(2);
    std::string hex1 = to_hex(std::span<const uint8_t, 32>(ns1));
    std::string hex2 = to_hex(std::span<const uint8_t, 32>(ns2));

    AccessControl acl({hex1}, {hex2}, std::span<const uint8_t, 32>(own_ns));

    // Reload: change client keys, keep peer keys same
    auto result = acl.reload({hex2}, {hex2});

    REQUIRE(result.client_added == 1);    // ns2
    REQUIRE(result.client_removed == 1);  // ns1
    REQUIRE(result.peer_added == 0);
    REQUIRE(result.peer_removed == 0);
}

TEST_CASE("reload with empty keys switches to open mode", "[access_control]") {
    auto own_ns = make_ns_hash(0);
    auto ns1 = make_ns_hash(1);
    std::string hex1 = to_hex(std::span<const uint8_t, 32>(ns1));

    AccessControl acl({hex1}, {hex1}, std::span<const uint8_t, 32>(own_ns));
    REQUIRE(acl.is_client_closed_mode());
    REQUIRE(acl.is_peer_closed_mode());

    auto result = acl.reload({}, {});
    REQUIRE_FALSE(acl.is_client_closed_mode());
    REQUIRE_FALSE(acl.is_peer_closed_mode());
    REQUIRE(result.client_removed == 1);
    REQUIRE(result.peer_removed == 1);
    REQUIRE(result.client_added == 0);
    REQUIRE(result.peer_added == 0);
}

TEST_CASE("reload from open to closed mode", "[access_control]") {
    auto own_ns = make_ns_hash(0);
    AccessControl acl({}, {}, std::span<const uint8_t, 32>(own_ns));
    REQUIRE_FALSE(acl.is_client_closed_mode());
    REQUIRE_FALSE(acl.is_peer_closed_mode());

    auto ns1 = make_ns_hash(1);
    std::string hex1 = to_hex(std::span<const uint8_t, 32>(ns1));

    auto result = acl.reload({hex1}, {hex1});
    REQUIRE(acl.is_client_closed_mode());
    REQUIRE(acl.is_peer_closed_mode());
    REQUIRE(result.client_added == 1);
    REQUIRE(result.peer_added == 1);
    REQUIRE(result.client_removed == 0);
    REQUIRE(result.peer_removed == 0);
}

TEST_CASE("is_peer_allowed with namespace derived from SHA3-256 of pubkey", "[access_control]") {
    // Generate a real identity to get a real pubkey -> namespace hash
    auto identity = chromatindb::identity::NodeIdentity::generate();
    auto own_ns_span = identity.namespace_id();  // span<const uint8_t, 32>

    // Generate another identity to test against
    auto peer = chromatindb::identity::NodeIdentity::generate();
    auto peer_ns = chromatindb::crypto::sha3_256(peer.public_key());
    std::string peer_hex = to_hex(std::span<const uint8_t, 32>(peer_ns));

    AccessControl acl({}, {peer_hex}, own_ns_span);

    // Peer should be allowed as peer
    REQUIRE(acl.is_peer_allowed(std::span<const uint8_t, 32>(peer_ns)));

    // A random third party should NOT be allowed as peer
    auto third = chromatindb::identity::NodeIdentity::generate();
    auto third_ns = chromatindb::crypto::sha3_256(third.public_key());
    REQUIRE_FALSE(acl.is_peer_allowed(std::span<const uint8_t, 32>(third_ns)));
}

TEST_CASE("reload correctly changes is_peer_allowed results", "[access_control]") {
    auto own_ns = make_ns_hash(0);
    auto ns1 = make_ns_hash(1);
    auto ns2 = make_ns_hash(2);
    std::string hex1 = to_hex(std::span<const uint8_t, 32>(ns1));
    std::string hex2 = to_hex(std::span<const uint8_t, 32>(ns2));

    AccessControl acl({}, {hex1}, std::span<const uint8_t, 32>(own_ns));

    REQUIRE(acl.is_peer_allowed(std::span<const uint8_t, 32>(ns1)));
    REQUIRE_FALSE(acl.is_peer_allowed(std::span<const uint8_t, 32>(ns2)));

    // Reload: remove ns1, add ns2
    acl.reload({}, {hex2});

    REQUIRE_FALSE(acl.is_peer_allowed(std::span<const uint8_t, 32>(ns1)));
    REQUIRE(acl.is_peer_allowed(std::span<const uint8_t, 32>(ns2)));
}
