#include <gtest/gtest.h>

#include "crypto/crypto.h"
#include "kademlia/kademlia.h"
#include "kademlia/node_id.h"
#include "kademlia/routing_table.h"
#include "kademlia/tcp_transport.h"
#include "replication/repl_log.h"
#include "storage/storage.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace chromatin::kademlia;
using namespace chromatin::crypto;
using namespace chromatin::replication;
using namespace chromatin::storage;

// ---------------------------------------------------------------------------
// Test infrastructure: in-process test node
// ---------------------------------------------------------------------------

struct TestNode {
    KeyPair keypair;
    NodeInfo info;
    std::unique_ptr<TcpTransport> transport;
    std::unique_ptr<RoutingTable> table;
    std::unique_ptr<Storage> storage;
    std::unique_ptr<ReplLog> repl_log;
    std::unique_ptr<Kademlia> kad;
    std::thread recv_thread;
    std::filesystem::path db_path;
    std::atomic<bool> running{false};

    ~TestNode() {
        stop();
    }

    void stop() {
        if (running.load()) {
            transport->stop();
            if (recv_thread.joinable()) {
                recv_thread.join();
            }
            running.store(false);
        }
    }

    void start_recv() {
        running.store(true);
        recv_thread = std::thread([this]() {
            transport->run([this](const Message& msg, const std::string& from_addr, uint16_t from_port) {
                kad->handle_message(msg, from_addr, from_port);
            });
        });
    }
};

class KademliaTest : public ::testing::Test {
protected:
    std::vector<std::unique_ptr<TestNode>> nodes_;

    void TearDown() override {
        // Stop all nodes first, then clean up
        for (auto& node : nodes_) {
            node->stop();
        }
        for (auto& node : nodes_) {
            node->storage.reset();
            std::filesystem::remove_all(node->db_path);
        }
        nodes_.clear();
    }

    TestNode& create_node(int name_pow_difficulty = 8) {
        auto node = std::make_unique<TestNode>();

        node->keypair = generate_keypair();
        NodeId nid = NodeId::from_pubkey(node->keypair.public_key);

        // Create temp storage
        node->db_path = std::filesystem::temp_directory_path() /
                        ("chromatin_kad_test_" + std::to_string(reinterpret_cast<uintptr_t>(node.get())));
        std::filesystem::create_directories(node->db_path);
        node->storage = std::make_unique<Storage>(node->db_path / "test.mdbx");

        // Create transport on ephemeral port
        node->transport = std::make_unique<TcpTransport>("127.0.0.1", 0);
        uint16_t tcp_port = node->transport->local_port();

        // Build NodeInfo
        node->info.id = nid;
        node->info.address = "127.0.0.1";
        node->info.tcp_port = tcp_port;
        node->info.ws_port = 0;
        node->info.pubkey = node->keypair.public_key;
        node->info.last_seen = std::chrono::steady_clock::now();

        // Create routing table
        node->table = std::make_unique<RoutingTable>();

        // Create replication log
        node->repl_log = std::make_unique<ReplLog>(*node->storage);

        // Create kademlia engine
        node->kad = std::make_unique<Kademlia>(
            node->info, *node->transport, *node->table, *node->storage,
            *node->repl_log, node->keypair);
        node->kad->set_name_pow_difficulty(name_pow_difficulty);

        nodes_.push_back(std::move(node));
        return *nodes_.back();
    }

    // Start the recv loop on all created nodes
    void start_all() {
        for (auto& node : nodes_) {
            node->start_recv();
        }
        // Give recv loops time to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Build a name record binary blob per PROTOCOL-SPEC.md section 3
    static std::vector<uint8_t> build_name_record(
        const std::string& name,
        const Hash& fingerprint,
        uint64_t pow_nonce,
        uint64_t sequence,
        const KeyPair& user_keypair)
    {
        std::vector<uint8_t> record;

        // name_length (1 byte)
        record.push_back(static_cast<uint8_t>(name.size()));

        // name
        record.insert(record.end(), name.begin(), name.end());

        // fingerprint (32 bytes)
        record.insert(record.end(), fingerprint.begin(), fingerprint.end());

        // pow_nonce (8 bytes BE)
        for (int i = 7; i >= 0; --i) {
            record.push_back(static_cast<uint8_t>((pow_nonce >> (i * 8)) & 0xFF));
        }

        // sequence (8 bytes BE)
        for (int i = 7; i >= 0; --i) {
            record.push_back(static_cast<uint8_t>((sequence >> (i * 8)) & 0xFF));
        }

        // Sign everything so far
        auto sig = sign(record, user_keypair.secret_key);

        // sig_length (2 bytes BE)
        uint16_t sig_len = static_cast<uint16_t>(sig.size());
        record.push_back(static_cast<uint8_t>((sig_len >> 8) & 0xFF));
        record.push_back(static_cast<uint8_t>(sig_len & 0xFF));

        // signature
        record.insert(record.end(), sig.begin(), sig.end());

        return record;
    }

    // Brute-force find a valid PoW nonce for a name at given difficulty
    static uint64_t find_pow_nonce(const std::string& name, const Hash& fingerprint, int difficulty) {
        std::string prefix = "chromatin:name:";
        std::vector<uint8_t> preimage;
        preimage.insert(preimage.end(), prefix.begin(), prefix.end());
        preimage.insert(preimage.end(), name.begin(), name.end());
        preimage.insert(preimage.end(), fingerprint.begin(), fingerprint.end());

        for (uint64_t nonce = 0; nonce < 1000000; ++nonce) {
            if (verify_pow(preimage, nonce, difficulty)) {
                return nonce;
            }
        }
        // Should not reach here for low difficulty
        ADD_FAILURE() << "Could not find PoW nonce within 1M attempts for difficulty " << difficulty;
        return 0;
    }

    // Compute name storage key: SHA3-256("name:" || name)
    static Hash name_key(const std::string& name) {
        std::vector<uint8_t> name_bytes(name.begin(), name.end());
        return sha3_256_prefixed("name:", name_bytes);
    }
};

// ---------------------------------------------------------------------------
// Test 1: ReplicationFactor — single node, R >= 1
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, ReplicationFactor) {
    auto& n1 = create_node();
    EXPECT_GE(n1.kad->replication_factor(), 1u);
    EXPECT_EQ(n1.kad->replication_factor(), 1u); // Single node -> min(3, 1) = 1
}

// ---------------------------------------------------------------------------
// Test 2: BootstrapAddsNodes — 2 nodes discover each other
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, BootstrapAddsNodes) {
    auto& n1 = create_node();
    auto& n2 = create_node();

    start_all();

    // n2 bootstraps from n1
    n2.kad->bootstrap({{"127.0.0.1", n1.info.tcp_port}});

    // Wait for FIND_NODE -> NODES exchange
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // n1 should know about n2 (added when handling FIND_NODE)
    EXPECT_GE(n1.table->size(), 1u) << "n1 should have discovered n2";

    // n2 should know about n1 (from NODES response)
    EXPECT_GE(n2.table->size(), 1u) << "n2 should have discovered n1";
}

// ---------------------------------------------------------------------------
// Test 3: ThreeNodeBootstrap — 3 nodes all discover each other
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, ThreeNodeBootstrap) {
    auto& n1 = create_node();
    auto& n2 = create_node();
    auto& n3 = create_node();

    start_all();

    // n2 bootstraps from n1
    n2.kad->bootstrap({{"127.0.0.1", n1.info.tcp_port}});
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // n3 bootstraps from n1 (n1 will tell n3 about n2)
    n3.kad->bootstrap({{"127.0.0.1", n1.info.tcp_port}});
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // n2 re-bootstraps to discover n3 (n1 now knows about n3)
    n2.kad->bootstrap({{"127.0.0.1", n1.info.tcp_port}});
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // n1 should know about n2 and n3
    EXPECT_GE(n1.table->size(), 2u) << "n1 should know about n2 and n3";

    // n2 should know about n1 and n3 (after re-bootstrap)
    EXPECT_GE(n2.table->size(), 2u) << "n2 should know about n1 and n3";

    // n3 should know about n1 and n2 (n1 sent all known nodes including n2)
    EXPECT_GE(n3.table->size(), 2u) << "n3 should know about n1 and n2";

    // Replication factor should be 3 for all nodes
    EXPECT_EQ(n1.kad->replication_factor(), 3u);
    EXPECT_EQ(n2.kad->replication_factor(), 3u);
    EXPECT_EQ(n3.kad->replication_factor(), 3u);
}

// ---------------------------------------------------------------------------
// Test 4: StoreAndFindValue — store on n1, find from n2
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, StoreAndFindValue) {
    auto& n1 = create_node();
    auto& n2 = create_node();

    start_all();

    // Bootstrap n2 from n1
    n2.kad->bootstrap({{"127.0.0.1", n1.info.tcp_port}});
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // Store a profile value via n1 (using data_type 0x00 = profile)
    Hash key{};
    key.fill(0x42);
    std::vector<uint8_t> value = {0xDE, 0xAD, 0xBE, 0xEF};

    // Store directly into storage on n1 (bypassing responsibility check for this test)
    n1.storage->put(TABLE_PROFILES, key, value);

    // Now n2 sends FIND_VALUE to n1
    // Build FIND_VALUE payload
    std::vector<uint8_t> fv_payload(key.begin(), key.end());
    Message fv_msg;
    fv_msg.type = MessageType::FIND_VALUE;
    fv_msg.sender = n2.info.id;
    fv_msg.sender_port = n2.info.tcp_port;
    fv_msg.payload = fv_payload;
    sign_message(fv_msg, n2.keypair.secret_key);

    // Track received VALUE response
    std::atomic<bool> got_value{false};
    std::vector<uint8_t> received_value;

    // Stop n2's recv thread and restart with a custom handler
    n2.stop();
    n2.recv_thread = std::thread([&]() {
        n2.transport->run([&](const Message& msg, const std::string& from_addr, uint16_t from_port) {
            if (msg.type == MessageType::VALUE) {
                // Parse VALUE payload: [32 key][1 found][4 vlen][value]
                if (msg.payload.size() >= 37) {
                    uint8_t found = msg.payload[32];
                    if (found == 0x01) {
                        uint32_t vlen = (static_cast<uint32_t>(msg.payload[33]) << 24)
                                      | (static_cast<uint32_t>(msg.payload[34]) << 16)
                                      | (static_cast<uint32_t>(msg.payload[35]) << 8)
                                      | static_cast<uint32_t>(msg.payload[36]);
                        if (msg.payload.size() >= 37 + vlen) {
                            received_value.assign(
                                msg.payload.begin() + 37,
                                msg.payload.begin() + 37 + vlen);
                            got_value.store(true);
                        }
                    }
                }
                n2.transport->stop();
            } else {
                n2.kad->handle_message(msg, from_addr, from_port);
            }
        });
    });
    n2.running.store(true);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Send FIND_VALUE from n2 to n1
    n2.transport->send("127.0.0.1", n1.info.tcp_port, fv_msg);

    // Wait for response
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // Clean up n2's thread
    n2.transport->stop();
    if (n2.recv_thread.joinable()) {
        n2.recv_thread.join();
    }
    n2.running.store(false);

    ASSERT_TRUE(got_value.load()) << "n2 should have received VALUE response from n1";
    EXPECT_EQ(received_value, value);
}

// ---------------------------------------------------------------------------
// Test 5: ResponsibleNodes — 3 nodes, all responsible for a key (R=3)
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, ResponsibleNodes) {
    auto& n1 = create_node();
    auto& n2 = create_node();
    auto& n3 = create_node();

    start_all();

    // Bootstrap so all nodes know each other
    n2.kad->bootstrap({{"127.0.0.1", n1.info.tcp_port}});
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    n3.kad->bootstrap({{"127.0.0.1", n1.info.tcp_port}});
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // With 3 nodes and R=3, all nodes should be responsible for any key
    Hash key{};
    key.fill(0xAA);

    auto resp = n1.kad->responsible_nodes(key);
    EXPECT_EQ(resp.size(), 3u);
    EXPECT_TRUE(n1.kad->is_responsible(key));
    EXPECT_TRUE(n2.kad->is_responsible(key));
    EXPECT_TRUE(n3.kad->is_responsible(key));
}

// ---------------------------------------------------------------------------
// Test 6: NameRegistrationValid — valid name record with real PoW + ML-DSA sig
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, NameRegistrationValid) {
    auto& n1 = create_node(8); // Low PoW difficulty for testing

    start_all();

    // Generate a "user" keypair (separate from node keypair)
    KeyPair user_kp = generate_keypair();
    Hash user_fp = sha3_256(user_kp.public_key);

    std::string name = "alice";
    uint64_t nonce = find_pow_nonce(name, user_fp, 8);
    uint64_t sequence = 1;

    auto record = build_name_record(name, user_fp, nonce, sequence, user_kp);
    Hash key = name_key(name);

    // Store locally (single node is always responsible)
    bool ok = n1.kad->store(key, 0x01, record);
    EXPECT_TRUE(ok);

    // Verify it was stored
    auto result = n1.storage->get(TABLE_NAMES, key);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, record);
}

// ---------------------------------------------------------------------------
// Test 7: NameRegistrationInvalidPow — bad nonce gets rejected
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, NameRegistrationInvalidPow) {
    auto& n1 = create_node(8);

    start_all();

    KeyPair user_kp = generate_keypair();
    Hash user_fp = sha3_256(user_kp.public_key);

    std::string name = "badpow";

    // Use a nonce that almost certainly won't pass PoW at difficulty 8
    // We pick 0xDEADBEEF which is extremely unlikely to produce 8 leading zero bits
    uint64_t bad_nonce = 0xDEADBEEFDEADBEEF;
    uint64_t sequence = 1;

    auto record = build_name_record(name, user_fp, bad_nonce, sequence, user_kp);
    Hash key = name_key(name);

    bool ok = n1.kad->store(key, 0x01, record);
    // The store should "succeed" from the caller's perspective (it tried to store),
    // but validate_name_record should reject it, so nothing should be in storage.
    // Actually, store() returns false if it couldn't store anywhere
    // Let's check storage directly
    auto result = n1.storage->get(TABLE_NAMES, key);
    EXPECT_FALSE(result.has_value()) << "Name with invalid PoW should not be stored";
}

// ---------------------------------------------------------------------------
// Test 8: NameRegistrationFirstClaimWins — different fingerprint gets rejected
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, NameRegistrationFirstClaimWins) {
    auto& n1 = create_node(8);

    start_all();

    // First user registers the name
    KeyPair user1_kp = generate_keypair();
    Hash user1_fp = sha3_256(user1_kp.public_key);
    std::string name = "claimed";
    uint64_t nonce1 = find_pow_nonce(name, user1_fp, 8);

    auto record1 = build_name_record(name, user1_fp, nonce1, 1, user1_kp);
    Hash key = name_key(name);

    bool ok1 = n1.kad->store(key, 0x01, record1);
    EXPECT_TRUE(ok1);

    auto result1 = n1.storage->get(TABLE_NAMES, key);
    ASSERT_TRUE(result1.has_value());

    // Second user tries to register the same name with a different fingerprint
    KeyPair user2_kp = generate_keypair();
    Hash user2_fp = sha3_256(user2_kp.public_key);
    uint64_t nonce2 = find_pow_nonce(name, user2_fp, 8);

    auto record2 = build_name_record(name, user2_fp, nonce2, 1, user2_kp);

    // This should fail (first claim wins)
    n1.kad->store(key, 0x01, record2);

    // The stored value should still be the first user's record
    auto result2 = n1.storage->get(TABLE_NAMES, key);
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(*result2, record1) << "Name should still belong to first claimant";
}

// ---------------------------------------------------------------------------
// Test 9: WriteQuorum — W = min(2, R)
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, WriteQuorum) {
    auto& n1 = create_node();
    EXPECT_EQ(n1.kad->write_quorum(), 1u); // Single node: min(2, 1) = 1

    auto& n2 = create_node();
    start_all();

    n2.kad->bootstrap({{"127.0.0.1", n1.info.tcp_port}});
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // 2 nodes: R=2, W=min(2, 2) = 2
    EXPECT_EQ(n1.kad->write_quorum(), 2u);
    EXPECT_EQ(n2.kad->write_quorum(), 2u);
}

// ---------------------------------------------------------------------------
// Test 10: StoreAckSent — handle_store sends STORE_ACK back
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, StoreAckSent) {
    auto& n1 = create_node(8);
    auto& n2 = create_node(8);

    start_all();

    // Bootstrap
    n2.kad->bootstrap({{"127.0.0.1", n1.info.tcp_port}});
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // Store a profile on n1 directly, then send a STORE message from n2 to n1
    Hash key{};
    key.fill(0x55);
    std::vector<uint8_t> value = {0xCA, 0xFE};

    // Build STORE payload: [32 key][1 data_type=0x00][4 vlen][value]
    std::vector<uint8_t> store_payload;
    store_payload.insert(store_payload.end(), key.begin(), key.end());
    store_payload.push_back(0x00); // profile
    uint32_t vlen = static_cast<uint32_t>(value.size());
    store_payload.push_back(static_cast<uint8_t>((vlen >> 24) & 0xFF));
    store_payload.push_back(static_cast<uint8_t>((vlen >> 16) & 0xFF));
    store_payload.push_back(static_cast<uint8_t>((vlen >> 8) & 0xFF));
    store_payload.push_back(static_cast<uint8_t>(vlen & 0xFF));
    store_payload.insert(store_payload.end(), value.begin(), value.end());

    // Track received STORE_ACK
    std::atomic<bool> got_ack{false};
    uint8_t ack_status = 0xFF;

    // Restart n2 with custom handler to capture STORE_ACK
    n2.stop();
    n2.recv_thread = std::thread([&]() {
        n2.transport->run([&](const Message& msg, const std::string& from_addr, uint16_t from_port) {
            if (msg.type == MessageType::STORE_ACK) {
                if (msg.payload.size() >= 33) {
                    ack_status = msg.payload[32];
                    got_ack.store(true);
                }
                n2.transport->stop();
            } else {
                n2.kad->handle_message(msg, from_addr, from_port);
            }
        });
    });
    n2.running.store(true);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Send STORE from n2 to n1
    Message store_msg;
    store_msg.type = MessageType::STORE;
    store_msg.sender = n2.info.id;
    store_msg.sender_port = n2.info.tcp_port;
    store_msg.payload = store_payload;
    sign_message(store_msg, n2.keypair.secret_key);
    n2.transport->send("127.0.0.1", n1.info.tcp_port, store_msg);

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    n2.transport->stop();
    if (n2.recv_thread.joinable()) n2.recv_thread.join();
    n2.running.store(false);

    ASSERT_TRUE(got_ack.load()) << "n2 should have received STORE_ACK from n1";
    EXPECT_EQ(ack_status, 0x00) << "STORE_ACK status should be OK";
}

// ---------------------------------------------------------------------------
// Test 11: PendingStoreTracking — store() creates and resolves pending entries
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, PendingStoreTracking) {
    auto& n1 = create_node(8);
    auto& n2 = create_node(8);

    start_all();

    // Bootstrap so both nodes know each other (R=2)
    n2.kad->bootstrap({{"127.0.0.1", n1.info.tcp_port}});
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    ASSERT_EQ(n1.kad->replication_factor(), 2u);

    // Store a profile via n1 — it will store locally and STORE to n2
    Hash key{};
    key.fill(0x77);
    std::vector<uint8_t> value = {0xBE, 0xEF};

    bool ok = n1.kad->store(key, 0x00, value);
    ASSERT_TRUE(ok);

    // Immediately after store(), there should be a pending entry
    auto status = n1.kad->pending_store_status(key);
    ASSERT_TRUE(status.has_value()) << "Should have a pending store entry";
    EXPECT_EQ(status->expected, 1u) << "Expected 1 remote STORE";
    EXPECT_EQ(status->acked, 0u) << "No ACKs received yet";
    EXPECT_TRUE(status->local_stored);

    // Wait for n2 to process the STORE and send STORE_ACK back (poll for reliability)
    bool resolved = false;
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto s = n1.kad->pending_store_status(key);
        if (!s.has_value()) {
            resolved = true;
            break;
        }
    }

    // After ACK, the pending entry should be resolved (removed)
    EXPECT_TRUE(resolved) << "Pending store should be resolved after STORE_ACK";

    // Verify n2 actually stored the data
    auto n2_data = n2.storage->get(TABLE_PROFILES, key);
    ASSERT_TRUE(n2_data.has_value());
    EXPECT_EQ(*n2_data, value);
}

// ---------------------------------------------------------------------------
// Test 12: ThreeNodeQuorum — W=2 of R=3, quorum reached after 1 remote ACK
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, ThreeNodeQuorum) {
    auto& n1 = create_node(8);
    auto& n2 = create_node(8);
    auto& n3 = create_node(8);

    start_all();

    // Bootstrap all nodes
    n2.kad->bootstrap({{"127.0.0.1", n1.info.tcp_port}});
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    n3.kad->bootstrap({{"127.0.0.1", n1.info.tcp_port}});
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    n2.kad->bootstrap({{"127.0.0.1", n1.info.tcp_port}});
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    ASSERT_EQ(n1.kad->replication_factor(), 3u);
    ASSERT_EQ(n1.kad->write_quorum(), 2u); // W = min(2, 3) = 2

    // Store a profile via n1
    Hash key{};
    key.fill(0x88);
    std::vector<uint8_t> value = {0xAB, 0xCD};

    bool ok = n1.kad->store(key, 0x00, value);
    ASSERT_TRUE(ok);

    // n1 stored locally + sent to 2 remote nodes
    auto status = n1.kad->pending_store_status(key);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->expected, 2u);
    EXPECT_TRUE(status->local_stored);

    // Wait for quorum (W=2: local_stored + at least 1 remote ACK)
    bool quorum_met = false;
    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto s = n1.kad->pending_store_status(key);
        if (!s.has_value()) {
            quorum_met = true;  // fully cleared = quorum definitely met
            break;
        }
        size_t confirmed = s->acked + (s->local_stored ? 1 : 0);
        if (confirmed >= n1.kad->write_quorum()) {
            quorum_met = true;
            break;
        }
    }
    EXPECT_TRUE(quorum_met) << "Write quorum should be met (W=" << n1.kad->write_quorum() << ")";

    // Verify all 3 nodes have the data
    EXPECT_TRUE(n1.storage->get(TABLE_PROFILES, key).has_value());
    EXPECT_TRUE(n2.storage->get(TABLE_PROFILES, key).has_value());
    EXPECT_TRUE(n3.storage->get(TABLE_PROFILES, key).has_value());
}

// ---------------------------------------------------------------------------
// Test 13: SyncBetweenNodes — store on n1, n2 sends SYNC_REQ, gets data
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, SyncBetweenNodes) {
    auto& n1 = create_node(8);
    auto& n2 = create_node(8);
    auto& n3 = create_node(8);

    start_all();

    // Bootstrap all nodes so they know each other
    n2.kad->bootstrap({{"127.0.0.1", n1.info.tcp_port}});
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    n3.kad->bootstrap({{"127.0.0.1", n1.info.tcp_port}});
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    // Re-bootstrap so n2 knows n3
    n2.kad->bootstrap({{"127.0.0.1", n1.info.tcp_port}});
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // Store a name record on node 1 directly (simulating a STORE)
    KeyPair user_kp = generate_keypair();
    Hash user_fp = sha3_256(user_kp.public_key);
    std::string name = "synctest";
    uint64_t nonce = find_pow_nonce(name, user_fp, 8);
    auto record = build_name_record(name, user_fp, nonce, 1, user_kp);
    Hash key = name_key(name);

    // Store directly on n1 (local storage + repl_log)
    n1.storage->put(TABLE_NAMES, key, record);
    n1.repl_log->append(key, Op::ADD, record);

    // Verify n1 has the data and repl log entry
    ASSERT_TRUE(n1.storage->get(TABLE_NAMES, key).has_value());
    ASSERT_EQ(n1.repl_log->current_seq(key), 1u);

    // Verify n2 does NOT have the data yet
    ASSERT_FALSE(n2.storage->get(TABLE_NAMES, key).has_value());

    // Node 2 sends SYNC_REQ to node 1 for the key
    // SYNC_REQ payload: [32 bytes: key][8 bytes BE: after_seq=0]
    std::vector<uint8_t> sync_payload;
    sync_payload.insert(sync_payload.end(), key.begin(), key.end());
    for (int i = 7; i >= 0; --i) {
        sync_payload.push_back(static_cast<uint8_t>((static_cast<uint64_t>(0) >> (i * 8)) & 0xFF));
    }

    Message sync_msg;
    sync_msg.type = MessageType::SYNC_REQ;
    sync_msg.sender = n2.info.id;
    sync_msg.sender_port = n2.info.tcp_port;
    sync_msg.payload = sync_payload;
    sign_message(sync_msg, n2.keypair.secret_key);

    n2.transport->send("127.0.0.1", n1.info.tcp_port, sync_msg);

    // Wait for SYNC_REQ -> SYNC_RESP exchange
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // Verify node 2 now has the data
    auto n2_data = n2.storage->get(TABLE_NAMES, key);
    ASSERT_TRUE(n2_data.has_value()) << "n2 should have received the name record via SYNC";
    EXPECT_EQ(*n2_data, record) << "Synced data should match original";

    // Verify n2's repl log also has the entry
    EXPECT_EQ(n2.repl_log->current_seq(key), 1u);
}

// ---------------------------------------------------------------------------
// Test 14: TickRefreshesRoutingTable — tick() discovers late-joining node
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, TickRefreshesRoutingTable) {
    auto& n1 = create_node();
    auto& n2 = create_node();

    // Only start n1 initially
    n1.start_recv();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // n1 bootstraps from n2's port, but n2 isn't receiving yet — no discovery
    n1.kad->set_bootstrap_addrs({{"127.0.0.1", n2.info.tcp_port}});
    EXPECT_EQ(n1.table->size(), 0u) << "n1 should not know any nodes yet";

    // Now start n2
    n2.start_recv();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Call tick() on n1 — should trigger re-bootstrap and discover n2
    n1.kad->tick();

    // Wait for FIND_NODE -> NODES exchange
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    EXPECT_GE(n1.table->size(), 1u) << "tick() should have discovered n2 via re-bootstrap";
}

// ---------------------------------------------------------------------------
// Test 15: PongUpdatesLastSeen — PONG updates last_seen in routing table
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, PongUpdatesLastSeen) {
    auto& n1 = create_node();
    auto& n2 = create_node();

    start_all();

    // Bootstrap so they know each other
    n2.kad->bootstrap({{"127.0.0.1", n1.info.tcp_port}});
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // Record n2's view of n1's last_seen
    auto before = n1.table->find(n2.info.id);
    ASSERT_TRUE(before.has_value()) << "n1 should know about n2";
    auto old_last_seen = before->last_seen;

    // Wait a bit so the timestamp will differ
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // n1 sends PING to n2, n2 responds with PONG, PONG handler updates n2 in n1's table
    Message ping_msg;
    ping_msg.type = MessageType::PING;
    ping_msg.sender = n1.info.id;
    ping_msg.sender_port = n1.info.tcp_port;
    ping_msg.payload = {};
    sign_message(ping_msg, n1.keypair.secret_key);
    n1.transport->send("127.0.0.1", n2.info.tcp_port, ping_msg);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    auto after = n1.table->find(n2.info.id);
    ASSERT_TRUE(after.has_value());
    EXPECT_GT(after->last_seen, old_last_seen) << "PONG should have updated last_seen";
}

// ---------------------------------------------------------------------------
// Test 16: StaleNodeEviction — evict nodes not seen for > threshold
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, StaleNodeEviction) {
    auto& n1 = create_node();

    // Manually add a "stale" node to n1's routing table with old timestamp
    NodeInfo stale;
    stale.id = NodeId::from_pubkey(generate_keypair().public_key);
    stale.address = "127.0.0.1";
    stale.tcp_port = 9999;
    stale.ws_port = 0;
    stale.last_seen = std::chrono::steady_clock::now() - std::chrono::seconds(200);
    n1.table->add_or_update(stale);

    ASSERT_EQ(n1.table->size(), 1u);

    // evict_older_than should remove it
    auto cutoff = std::chrono::steady_clock::now() - std::chrono::seconds(120);
    n1.table->evict_older_than(cutoff);

    EXPECT_EQ(n1.table->size(), 0u) << "Stale node should have been evicted";
}

// ---------------------------------------------------------------------------
// Test 17: PingUpdatesRoutingTable — handle_ping adds sender to routing table
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, PingUpdatesRoutingTable) {
    auto& n1 = create_node();
    auto& n2 = create_node();

    start_all();

    // n1 doesn't know about n2 yet
    EXPECT_EQ(n1.table->size(), 0u);

    // n2 sends PING to n1 — handle_ping should add n2 to n1's routing table
    Message ping_msg;
    ping_msg.type = MessageType::PING;
    ping_msg.sender = n2.info.id;
    ping_msg.sender_port = n2.info.tcp_port;
    ping_msg.payload = {};
    sign_message(ping_msg, n2.keypair.secret_key);
    n2.transport->send("127.0.0.1", n1.info.tcp_port, ping_msg);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    EXPECT_GE(n1.table->size(), 1u) << "handle_ping should have added n2 to n1's routing table";
    auto found = n1.table->find(n2.info.id);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->tcp_port, n2.info.tcp_port);
}

// ---------------------------------------------------------------------------
// Test 18: OnStoreCallback — on_store fires after successful store
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, OnStoreCallback) {
    auto& n1 = create_node(8);
    auto& n2 = create_node(8);

    start_all();

    // Bootstrap so both nodes know each other
    n2.kad->bootstrap({{"127.0.0.1", n1.info.tcp_port}});
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // Set on_store callback on both nodes to capture events
    std::atomic<int> n1_cb_count{0};
    std::atomic<int> n2_cb_count{0};
    Hash n1_cb_key{};
    uint8_t n1_cb_dtype = 0xFF;
    Hash n2_cb_key{};
    uint8_t n2_cb_dtype = 0xFF;

    n1.kad->set_on_store([&](const Hash& key, uint8_t data_type,
                             std::span<const uint8_t> /*value*/) {
        n1_cb_key = key;
        n1_cb_dtype = data_type;
        n1_cb_count.fetch_add(1);
    });

    n2.kad->set_on_store([&](const Hash& key, uint8_t data_type,
                             std::span<const uint8_t> /*value*/) {
        n2_cb_key = key;
        n2_cb_dtype = data_type;
        n2_cb_count.fetch_add(1);
    });

    // Build a name record (same pattern as StoreNameRecord test)
    KeyPair user_kp = generate_keypair();
    Hash user_fp = sha3_256(user_kp.public_key);
    std::string name = "callbacktest";
    uint64_t nonce = find_pow_nonce(name, user_fp, 8);
    auto record = build_name_record(name, user_fp, nonce, 1, user_kp);
    Hash key = name_key(name);

    // Store via n1 — n1 stores locally (callback fires on n1's thread)
    // and sends STORE to n2 (callback fires on n2's accept thread)
    bool ok = n1.kad->store(key, 0x01, record);
    ASSERT_TRUE(ok);

    // Wait for the remote STORE to arrive at n2 and fire its callback
    bool both_fired = false;
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (n1_cb_count.load() >= 1 && n2_cb_count.load() >= 1) {
            both_fired = true;
            break;
        }
    }

    // n1 is responsible (it initiated the store and stores locally)
    EXPECT_GE(n1_cb_count.load(), 1) << "on_store should have fired on n1 (local store)";
    EXPECT_EQ(n1_cb_key, key);
    EXPECT_EQ(n1_cb_dtype, 0x01);

    // n2 should also have received the STORE and fired its callback
    EXPECT_GE(n2_cb_count.load(), 1) << "on_store should have fired on n2 (remote store)";
    EXPECT_EQ(n2_cb_key, key);
    EXPECT_EQ(n2_cb_dtype, 0x01);

    EXPECT_TRUE(both_fired) << "Callbacks should have fired on both nodes";
}
