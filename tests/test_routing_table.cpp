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
    RoutingTable rt(256, 0);  // disable subnet limit

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
    RoutingTable rt(256, 0);  // disable subnet limit

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

TEST(RoutingTable, SizeLimit) {
    RoutingTable rt(256, 0);  // disable subnet limit for size-cap test
    auto base_time = std::chrono::steady_clock::now();

    // Add 300 nodes with increasing timestamps so the first nodes are the
    // oldest.
    std::vector<NodeId> ids;
    for (int i = 0; i < 300; ++i) {
        std::string name = "node-sizecap-" + std::to_string(i);
        auto node = make_node(name);
        node.last_seen = base_time + std::chrono::seconds(i);
        ids.push_back(node.id);
        rt.add_or_update(std::move(node));
    }

    // Table should be capped at MAX_NODES
    EXPECT_EQ(rt.size(), rt.max_nodes());

    // The earliest nodes (0..43) should have been evicted because they had
    // the oldest last_seen timestamps. Nodes 44..299 remain (256 nodes).
    size_t evicted_count = 300 - rt.max_nodes(); // 44
    for (size_t i = 0; i < evicted_count; ++i) {
        EXPECT_FALSE(rt.find(ids[i]).has_value())
            << "Node " << i << " should have been evicted";
    }

    // The newest nodes should still be present
    for (size_t i = evicted_count; i < 300; ++i) {
        EXPECT_TRUE(rt.find(ids[i]).has_value())
            << "Node " << i << " should still be in the table";
    }
}

TEST(RoutingTable, SizeLimitUpdateDoesNotEvict) {
    RoutingTable rt(256, 0);  // disable subnet limit for size-cap test

    // Fill the table to capacity
    std::vector<NodeId> ids;
    for (size_t i = 0; i < rt.max_nodes(); ++i) {
        auto node = make_node("cap-node-" + std::to_string(i));
        ids.push_back(node.id);
        rt.add_or_update(std::move(node));
    }
    EXPECT_EQ(rt.size(), rt.max_nodes());

    // Updating an existing node should not change the size
    auto updated = make_node("cap-node-0", "10.0.0.99", 9999, 9999);
    rt.add_or_update(std::move(updated));
    EXPECT_EQ(rt.size(), rt.max_nodes());

    auto found = rt.find(ids[0]);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->address, "10.0.0.99");
}

TEST(RoutingTable, ClosestToCorrectness) {
    // Verify partial_sort gives the same result as a full sort would.
    RoutingTable rt(256, 0);  // disable subnet limit

    // Add 100 nodes
    std::vector<NodeInfo> all_nodes;
    for (int i = 0; i < 100; ++i) {
        auto node = make_node("perf-node-" + std::to_string(i));
        all_nodes.push_back(node);
        rt.add_or_update(std::move(node));
    }

    Hash target{};
    target[0] = 0xAB;
    target[1] = 0xCD;

    // Get the 3 closest via the routing table
    auto closest = rt.closest_to(target, 3);
    ASSERT_EQ(closest.size(), 3u);

    // Compute expected result with a full sort
    NodeId target_id;
    target_id.id = target;
    std::sort(all_nodes.begin(), all_nodes.end(),
        [&target_id](const NodeInfo& a, const NodeInfo& b) {
            return a.id.distance_to(target_id) < b.id.distance_to(target_id);
        });

    // The 3 closest should match
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(closest[i].id, all_nodes[i].id)
            << "Mismatch at position " << i;
    }

    // Verify the returned nodes are in sorted order
    for (size_t i = 1; i < closest.size(); ++i) {
        EXPECT_LE(closest[i - 1].id.distance_to(target_id),
                  closest[i].id.distance_to(target_id));
    }
}

TEST(RoutingTable, SubnetDiversity) {
    // max_per_subnet = 2 for easier testing
    RoutingTable rt(256, 2);

    // Add 2 nodes from subnet 10.0.1.x — should succeed
    auto n1 = make_node("subnet-a1", "10.0.1.1");
    auto n2 = make_node("subnet-a2", "10.0.1.2");
    rt.add_or_update(std::move(n1));
    rt.add_or_update(std::move(n2));
    EXPECT_EQ(rt.size(), 2u);

    // 3rd node from same subnet — should be rejected
    auto n3 = make_node("subnet-a3", "10.0.1.3");
    rt.add_or_update(std::move(n3));
    EXPECT_EQ(rt.size(), 2u);  // still 2

    // Node from different subnet — should succeed
    auto n4 = make_node("subnet-b1", "10.0.2.1");
    rt.add_or_update(std::move(n4));
    EXPECT_EQ(rt.size(), 3u);

    // Updating existing node (same ID, new address) should always work
    auto n1_updated = make_node("subnet-a1", "10.0.1.99");
    rt.add_or_update(std::move(n1_updated));
    EXPECT_EQ(rt.size(), 3u);
    auto found = rt.find(NodeId::from_pubkey(std::vector<uint8_t>{'s','u','b','n','e','t','-','a','1'}));
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->address, "10.0.1.99");
}

TEST(RoutingTable, SubnetDiversityIPv6) {
    RoutingTable rt(256, 2);

    // Add 2 nodes from same /48 prefix
    auto n1 = make_node("v6-a1", "2001:db8:1::1");
    auto n2 = make_node("v6-a2", "2001:db8:1::2");
    rt.add_or_update(std::move(n1));
    rt.add_or_update(std::move(n2));
    EXPECT_EQ(rt.size(), 2u);

    // 3rd from same /48 — rejected
    auto n3 = make_node("v6-a3", "2001:db8:1::3");
    rt.add_or_update(std::move(n3));
    EXPECT_EQ(rt.size(), 2u);

    // Different /48 — accepted
    auto n4 = make_node("v6-b1", "2001:db8:2::1");
    rt.add_or_update(std::move(n4));
    EXPECT_EQ(rt.size(), 3u);
}

TEST(RoutingTable, SubnetDiversityDisabled) {
    // max_per_subnet = 0 disables the check
    RoutingTable rt(256, 0);

    for (int i = 0; i < 10; ++i) {
        rt.add_or_update(make_node("same-subnet-" + std::to_string(i), "10.0.1." + std::to_string(i + 1)));
    }
    EXPECT_EQ(rt.size(), 10u);
}

TEST(RoutingTable, SubnetDiversityUsesTcpSourceIp) {
    // Verify that subnet diversity uses tcp_source_ip, not the self-reported
    // address. A Sybil attacker can fake external_address but not the TCP
    // source IP observed by the receiving node.
    RoutingTable rt(256, 2);

    // Add 2 nodes with tcp_source_ip in subnet 10.0.1.x
    auto n1 = make_node("tcp-src-a1", "192.168.1.1");
    n1.tcp_source_ip = "10.0.1.1";
    auto n2 = make_node("tcp-src-a2", "192.168.2.1");
    n2.tcp_source_ip = "10.0.1.2";
    rt.add_or_update(std::move(n1));
    rt.add_or_update(std::move(n2));
    EXPECT_EQ(rt.size(), 2u);

    // 3rd node claims different self-reported address but same TCP source subnet
    // — should be rejected based on tcp_source_ip
    auto n3 = make_node("tcp-src-a3", "172.16.0.1");
    n3.tcp_source_ip = "10.0.1.3";
    rt.add_or_update(std::move(n3));
    EXPECT_EQ(rt.size(), 2u);  // rejected

    // Node from different TCP source subnet — should succeed
    auto n4 = make_node("tcp-src-b1", "192.168.3.1");
    n4.tcp_source_ip = "10.0.2.1";
    rt.add_or_update(std::move(n4));
    EXPECT_EQ(rt.size(), 3u);
}

TEST(RoutingTable, SubnetDiversityFallsBackToAddress) {
    // When tcp_source_ip is empty (e.g. NODES response entries), the subnet
    // check should fall back to using the address field.
    RoutingTable rt(256, 2);

    auto n1 = make_node("fallback-a1", "10.0.1.1");
    auto n2 = make_node("fallback-a2", "10.0.1.2");
    rt.add_or_update(std::move(n1));
    rt.add_or_update(std::move(n2));
    EXPECT_EQ(rt.size(), 2u);

    // 3rd node without tcp_source_ip, same address subnet — rejected
    auto n3 = make_node("fallback-a3", "10.0.1.3");
    rt.add_or_update(std::move(n3));
    EXPECT_EQ(rt.size(), 2u);
}

TEST(RoutingTable, SubnetDiversityTcpSourceIpPreservedOnUpdate) {
    // Verify that updating an existing node preserves tcp_source_ip
    RoutingTable rt(256, 2);

    auto n1 = make_node("preserve-a1", "192.168.1.1");
    n1.tcp_source_ip = "10.0.1.1";
    NodeId id = n1.id;
    rt.add_or_update(std::move(n1));

    // Update the node without providing tcp_source_ip (e.g. from NODES response)
    auto n1_update = make_node("preserve-a1", "192.168.1.99");
    rt.add_or_update(std::move(n1_update));

    auto found = rt.find(id);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->address, "192.168.1.99");
    EXPECT_EQ(found->tcp_source_ip, "10.0.1.1");  // preserved
}
