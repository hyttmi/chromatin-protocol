#include <gtest/gtest.h>

#include "kademlia/routing_table.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

using namespace chromatin::kademlia;
using namespace chromatin::crypto;

namespace {

NodeInfo make_node(const std::string& name, const std::string& addr = "127.0.0.1",
                   uint16_t tcp = 5000, uint16_t ws = 8080) {
    std::vector<uint8_t> pk(name.begin(), name.end());
    NodeInfo info;
    info.id = NodeId::from_pubkey(pk);
    info.address = addr;
    info.tcp_port = tcp;
    info.ws_port = ws;
    info.pubkey = pk;
    info.last_seen = std::chrono::steady_clock::now();
    return info;
}

} // anonymous namespace

TEST(RoutingTable, AddAndFind) {
    RoutingTable rt;
    auto node = make_node("node-1");
    NodeId id = node.id;

    rt.add_or_update(std::move(node));
    auto found = rt.find(id);

    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->id, id);
    EXPECT_EQ(found->address, "127.0.0.1");
    EXPECT_EQ(found->tcp_port, 5000);
    EXPECT_EQ(found->ws_port, 8080);
}

TEST(RoutingTable, FindMissing) {
    RoutingTable rt;
    auto node = make_node("node-1");
    auto missing = make_node("node-missing");

    rt.add_or_update(std::move(node));
    auto found = rt.find(missing.id);
    EXPECT_FALSE(found.has_value());
}

TEST(RoutingTable, Remove) {
    RoutingTable rt;
    auto node = make_node("node-1");
    NodeId id = node.id;

    rt.add_or_update(std::move(node));
    EXPECT_EQ(rt.size(), 1u);

    rt.remove(id);
    EXPECT_EQ(rt.size(), 0u);
    EXPECT_FALSE(rt.find(id).has_value());
}

TEST(RoutingTable, UpdateExisting) {
    RoutingTable rt;
    auto node1 = make_node("node-1", "10.0.0.1", 5000, 8080);
    NodeId id = node1.id;

    rt.add_or_update(std::move(node1));
    EXPECT_EQ(rt.size(), 1u);

    // Update same node with new address
    auto node1_updated = make_node("node-1", "10.0.0.2", 6000, 9090);
    rt.add_or_update(std::move(node1_updated));

    // Size should not increase
    EXPECT_EQ(rt.size(), 1u);

    auto found = rt.find(id);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->address, "10.0.0.2");
    EXPECT_EQ(found->tcp_port, 6000);
    EXPECT_EQ(found->ws_port, 9090);
}

TEST(RoutingTable, ClosestToOrdering) {
    RoutingTable rt;

    // Add several nodes
    auto n1 = make_node("alpha");
    auto n2 = make_node("beta");
    auto n3 = make_node("gamma");
    auto n4 = make_node("delta");

    // Use the ID of n1 as the target key
    Hash target = n1.id.id;

    rt.add_or_update(n1);
    rt.add_or_update(n2);
    rt.add_or_update(n3);
    rt.add_or_update(n4);

    auto closest = rt.closest_to(target, 4);
    ASSERT_EQ(closest.size(), 4u);

    // First result should be n1 (distance 0 to itself)
    EXPECT_EQ(closest[0].id, n1.id);

    // Verify ordering: each node should be no farther than the next
    NodeId target_id;
    target_id.id = target;
    for (size_t i = 1; i < closest.size(); ++i) {
        EXPECT_LE(closest[i - 1].id.distance_to(target_id),
                  closest[i].id.distance_to(target_id));
    }
}

TEST(RoutingTable, ClosestToLimitedCount) {
    RoutingTable rt;

    rt.add_or_update(make_node("node-a"));
    rt.add_or_update(make_node("node-b"));
    rt.add_or_update(make_node("node-c"));
    rt.add_or_update(make_node("node-d"));
    rt.add_or_update(make_node("node-e"));

    Hash target{};
    target[0] = 0x42;

    auto closest = rt.closest_to(target, 3);
    EXPECT_EQ(closest.size(), 3u);
}

TEST(RoutingTable, ClosestToMoreThanAvailable) {
    RoutingTable rt;

    rt.add_or_update(make_node("only-node"));

    Hash target{};
    auto closest = rt.closest_to(target, 10);
    EXPECT_EQ(closest.size(), 1u);
}

TEST(RoutingTable, AllNodes) {
    RoutingTable rt;

    rt.add_or_update(make_node("node-1"));
    rt.add_or_update(make_node("node-2"));
    rt.add_or_update(make_node("node-3"));

    auto all = rt.all_nodes();
    EXPECT_EQ(all.size(), 3u);
}

TEST(RoutingTable, EmptyTable) {
    RoutingTable rt;
    EXPECT_EQ(rt.size(), 0u);
    EXPECT_TRUE(rt.all_nodes().empty());

    Hash target{};
    auto closest = rt.closest_to(target, 5);
    EXPECT_TRUE(closest.empty());
}
