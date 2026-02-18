#include <gtest/gtest.h>

#include "crypto/crypto.h"
#include "kademlia/node_id.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace chromatin::kademlia;
using namespace chromatin::crypto;

TEST(NodeId, FromPubkeyMatchesSHA3_256) {
    // Use a fake pubkey (just some bytes)
    std::string pubkey_str = "fake-public-key-bytes-for-testing";
    std::vector<uint8_t> pubkey(pubkey_str.begin(), pubkey_str.end());

    NodeId nid = NodeId::from_pubkey(pubkey);
    Hash expected = sha3_256(pubkey);

    EXPECT_EQ(nid.id, expected);
}

TEST(NodeId, DistanceToSelfIsZero) {
    std::string pubkey_str = "some-public-key";
    std::vector<uint8_t> pubkey(pubkey_str.begin(), pubkey_str.end());
    NodeId nid = NodeId::from_pubkey(pubkey);

    Hash dist = nid.distance_to(nid);
    Hash zero{};
    EXPECT_EQ(dist, zero);
}

TEST(NodeId, DistanceIsSymmetric) {
    std::string pk1_str = "pubkey-one";
    std::string pk2_str = "pubkey-two";
    std::vector<uint8_t> pk1(pk1_str.begin(), pk1_str.end());
    std::vector<uint8_t> pk2(pk2_str.begin(), pk2_str.end());

    NodeId n1 = NodeId::from_pubkey(pk1);
    NodeId n2 = NodeId::from_pubkey(pk2);

    EXPECT_EQ(n1.distance_to(n2), n2.distance_to(n1));
}

TEST(NodeId, DistanceToOtherIsNonZero) {
    std::string pk1_str = "pubkey-alpha";
    std::string pk2_str = "pubkey-beta";
    std::vector<uint8_t> pk1(pk1_str.begin(), pk1_str.end());
    std::vector<uint8_t> pk2(pk2_str.begin(), pk2_str.end());

    NodeId n1 = NodeId::from_pubkey(pk1);
    NodeId n2 = NodeId::from_pubkey(pk2);

    Hash dist = n1.distance_to(n2);
    Hash zero{};
    EXPECT_NE(dist, zero);
}

TEST(NodeId, Equality) {
    std::string pk_str = "same-key";
    std::vector<uint8_t> pk(pk_str.begin(), pk_str.end());

    NodeId n1 = NodeId::from_pubkey(pk);
    NodeId n2 = NodeId::from_pubkey(pk);

    EXPECT_EQ(n1, n2);
}

TEST(NodeId, InequalityDifferentKeys) {
    std::string pk1_str = "key-A";
    std::string pk2_str = "key-B";
    std::vector<uint8_t> pk1(pk1_str.begin(), pk1_str.end());
    std::vector<uint8_t> pk2(pk2_str.begin(), pk2_str.end());

    NodeId n1 = NodeId::from_pubkey(pk1);
    NodeId n2 = NodeId::from_pubkey(pk2);

    EXPECT_NE(n1, n2);
}

TEST(NodeId, HashFunctionProducesValue) {
    std::string pk_str = "hash-test-key";
    std::vector<uint8_t> pk(pk_str.begin(), pk_str.end());

    NodeId nid = NodeId::from_pubkey(pk);
    NodeIdHash hasher;

    // Just verify it doesn't crash and produces a value
    size_t h = hasher(nid);
    // Same node should produce same hash
    EXPECT_EQ(h, hasher(nid));
}
