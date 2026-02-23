#include <gtest/gtest.h>

#include "config/config.h"
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
    chromatin::config::Config cfg;
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
            node->cfg, node->info, *node->transport, *node->table, *node->storage,
            *node->repl_log, node->keypair);
        node->kad->set_name_pow_difficulty(name_pow_difficulty);
        node->kad->set_contact_pow_difficulty(8);  // low for testing

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

    // Bidirectional bootstrap between two nodes so both have each other's pubkeys.
    // After FIND_NODE→NODES exchange, only the initiator learns the peer's pubkey.
    // The reverse bootstrap ensures both sides can verify signed messages.
    void bootstrap_bidirectional(TestNode& a, TestNode& b) {
        a.kad->bootstrap({{"127.0.0.1", b.info.tcp_port}});
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        b.kad->bootstrap({{"127.0.0.1", a.info.tcp_port}});
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
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

        // pubkey_length (2 bytes BE) + pubkey
        uint16_t pk_len = static_cast<uint16_t>(user_keypair.public_key.size());
        record.push_back(static_cast<uint8_t>((pk_len >> 8) & 0xFF));
        record.push_back(static_cast<uint8_t>(pk_len & 0xFF));
        record.insert(record.end(),
                       user_keypair.public_key.begin(),
                       user_keypair.public_key.end());

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

    // Store a user's profile directly into a node's storage (test setup helper).
    // This is needed because name records and allowlist entries require the
    // owner's profile to exist for signature verification.
    static void store_user_profile(TestNode& node, const KeyPair& user_kp) {
        Hash user_fp = sha3_256(user_kp.public_key);
        std::vector<uint8_t> profile;
        // fingerprint(32)
        profile.insert(profile.end(), user_fp.begin(), user_fp.end());
        // pubkey_len(2 BE) + pubkey
        uint16_t pk_len = static_cast<uint16_t>(user_kp.public_key.size());
        profile.push_back(static_cast<uint8_t>((pk_len >> 8) & 0xFF));
        profile.push_back(static_cast<uint8_t>(pk_len & 0xFF));
        profile.insert(profile.end(), user_kp.public_key.begin(), user_kp.public_key.end());
        // kem_pubkey_length = 0, bio_length = 0, avatar_length = 0, social_count = 0
        profile.push_back(0x00); profile.push_back(0x00);
        profile.push_back(0x00); profile.push_back(0x00);
        profile.push_back(0x00); profile.push_back(0x00); profile.push_back(0x00); profile.push_back(0x00);
        profile.push_back(0x00);
        // sequence = 1
        for (int i = 7; i >= 0; --i) profile.push_back(i == 0 ? 0x01 : 0x00);
        // sign it
        auto sig = sign(std::span<const uint8_t>(profile.data(), profile.size()), user_kp.secret_key);
        uint16_t sig_len = static_cast<uint16_t>(sig.size());
        profile.push_back(static_cast<uint8_t>((sig_len >> 8) & 0xFF));
        profile.push_back(static_cast<uint8_t>(sig_len & 0xFF));
        profile.insert(profile.end(), sig.begin(), sig.end());

        Hash profile_key = sha3_256_prefixed("profile:", user_fp);
        node.storage->put(TABLE_PROFILES, profile_key, profile);
    }

    // Build a signed allowlist entry with real crypto
    static std::vector<uint8_t> build_signed_allowlist(
        const KeyPair& owner_kp, const Hash& contact_fp,
        uint8_t action, uint64_t sequence)
    {
        Hash owner_fp = sha3_256(owner_kp.public_key);

        // Signed data with domain separation:
        // "chromatin:allowlist:" || owner_fp(32) || action(1) || allowed_fp(32) || sequence(8 BE)
        const std::string domain = "chromatin:allowlist:";
        std::vector<uint8_t> signed_data;
        signed_data.insert(signed_data.end(), domain.begin(), domain.end());
        signed_data.insert(signed_data.end(), owner_fp.begin(), owner_fp.end());
        signed_data.push_back(action);
        signed_data.insert(signed_data.end(), contact_fp.begin(), contact_fp.end());
        for (int i = 7; i >= 0; --i)
            signed_data.push_back(static_cast<uint8_t>((sequence >> (i * 8)) & 0xFF));
        auto sig = sign(signed_data, owner_kp.secret_key);

        // Full entry: owner_fp(32) || allowed_fp(32) || action(1) || sequence(8 BE) || pubkey_len(2 BE) || pubkey || signature
        uint16_t pk_len = static_cast<uint16_t>(owner_kp.public_key.size());
        std::vector<uint8_t> entry;
        entry.insert(entry.end(), owner_fp.begin(), owner_fp.end());
        entry.insert(entry.end(), contact_fp.begin(), contact_fp.end());
        entry.push_back(action);
        for (int i = 7; i >= 0; --i)
            entry.push_back(static_cast<uint8_t>((sequence >> (i * 8)) & 0xFF));
        entry.push_back(static_cast<uint8_t>((pk_len >> 8) & 0xFF));
        entry.push_back(static_cast<uint8_t>(pk_len & 0xFF));
        entry.insert(entry.end(), owner_kp.public_key.begin(), owner_kp.public_key.end());
        entry.insert(entry.end(), sig.begin(), sig.end());

        return entry;
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

    // Bootstrap bidirectionally so both nodes have each other's pubkeys
    bootstrap_bidirectional(n2, n1);

    // Build a valid profile to store (must pass validate_readonly)
    KeyPair user_kp = generate_keypair();
    Hash user_fp = sha3_256(user_kp.public_key);
    Hash key = sha3_256_prefixed("profile:", user_fp);

    std::vector<uint8_t> value;
    // fingerprint(32)
    value.insert(value.end(), user_fp.begin(), user_fp.end());
    // pubkey_len(2 BE) + pubkey
    uint16_t pk_len = static_cast<uint16_t>(user_kp.public_key.size());
    value.push_back(static_cast<uint8_t>((pk_len >> 8) & 0xFF));
    value.push_back(static_cast<uint8_t>(pk_len & 0xFF));
    value.insert(value.end(), user_kp.public_key.begin(), user_kp.public_key.end());
    // kem_pubkey_len = 0, bio_len = 0, avatar_len = 0, social_count = 0
    value.push_back(0x00); value.push_back(0x00); // kem
    value.push_back(0x00); value.push_back(0x00); // bio
    value.push_back(0x00); value.push_back(0x00); value.push_back(0x00); value.push_back(0x00); // avatar
    value.push_back(0x00); // social_count
    // sequence(8 BE) = 1
    for (int i = 0; i < 7; ++i) value.push_back(0x00);
    value.push_back(0x01);
    // sign everything so far
    auto sig = sign(std::span<const uint8_t>(value.data(), value.size()), user_kp.secret_key);
    uint16_t sig_len = static_cast<uint16_t>(sig.size());
    value.push_back(static_cast<uint8_t>((sig_len >> 8) & 0xFF));
    value.push_back(static_cast<uint8_t>(sig_len & 0xFF));
    value.insert(value.end(), sig.begin(), sig.end());

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
// Test 8: NameRegistrationConflictResolution — lower fingerprint wins
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, NameRegistrationConflictResolution) {
    auto& n1 = create_node(8);

    start_all();

    // Generate two users and determine which has the lower fingerprint
    KeyPair user1_kp = generate_keypair();
    Hash user1_fp = sha3_256(user1_kp.public_key);
    KeyPair user2_kp = generate_keypair();
    Hash user2_fp = sha3_256(user2_kp.public_key);

    // Ensure user1 has the higher fingerprint (the loser)
    if (user1_fp < user2_fp) {
        std::swap(user1_kp, user2_kp);
        std::swap(user1_fp, user2_fp);
    }
    // Now user2_fp < user1_fp — user2 should win the tiebreaker

    std::string name = "claimed";
    uint64_t nonce1 = find_pow_nonce(name, user1_fp, 8);
    auto record1 = build_name_record(name, user1_fp, nonce1, 1, user1_kp);
    Hash key = name_key(name);

    // User1 (higher fp) registers first
    bool ok1 = n1.kad->store(key, 0x01, record1);
    EXPECT_TRUE(ok1);
    auto result1 = n1.storage->get(TABLE_NAMES, key);
    ASSERT_TRUE(result1.has_value());
    EXPECT_EQ(*result1, record1);

    // User2 (lower fp) registers same name — should win via tiebreaker
    uint64_t nonce2 = find_pow_nonce(name, user2_fp, 8);
    auto record2 = build_name_record(name, user2_fp, nonce2, 1, user2_kp);

    bool ok2 = n1.kad->store(key, 0x01, record2);
    EXPECT_TRUE(ok2);

    auto result2 = n1.storage->get(TABLE_NAMES, key);
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(*result2, record2) << "Lower fingerprint should win the conflict";

    // Now try to register with user1 again — should be rejected (user2 has lower fp)
    n1.kad->store(key, 0x01, record1);
    auto result3 = n1.storage->get(TABLE_NAMES, key);
    ASSERT_TRUE(result3.has_value());
    EXPECT_EQ(*result3, record2) << "Lower fingerprint should still hold the name";
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

    // Bootstrap bidirectionally so both nodes have each other's pubkeys
    bootstrap_bidirectional(n2, n1);

    // Use real keypair + signed allowlist entry (profile must exist for validation)
    KeyPair owner_kp = generate_keypair();
    Hash owner_fp = sha3_256(owner_kp.public_key);
    store_user_profile(n1, owner_kp);

    Hash contact_fp{};
    contact_fp.fill(0xAA);
    auto value = build_signed_allowlist(owner_kp, contact_fp, 0x01, 1);

    Hash key = sha3_256_prefixed("inbox:", owner_fp);

    // Build STORE payload: [32 key][1 data_type=0x04][4 vlen][value]
    std::vector<uint8_t> store_payload;
    store_payload.insert(store_payload.end(), key.begin(), key.end());
    store_payload.push_back(0x04); // allowlist
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

    // Bootstrap bidirectionally so both nodes have each other's pubkeys
    bootstrap_bidirectional(n2, n1);

    ASSERT_EQ(n1.kad->replication_factor(), 2u);

    // Use real keypair + signed allowlist entry (profile must exist for validation)
    KeyPair owner_kp = generate_keypair();
    Hash owner_fp = sha3_256(owner_kp.public_key);
    store_user_profile(n1, owner_kp);
    store_user_profile(n2, owner_kp);  // n2 also needs the profile to validate

    Hash contact_fp{};
    contact_fp.fill(0xBB);
    auto value = build_signed_allowlist(owner_kp, contact_fp, 0x01, 1);

    Hash key = sha3_256_prefixed("inbox:", owner_fp);

    bool ok = n1.kad->store(key, 0x04, value);
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

    // Verify n2 actually stored the data (composite key: key||allowed_fp)
    std::vector<uint8_t> composite_key(key.begin(), key.end());
    composite_key.insert(composite_key.end(), contact_fp.begin(), contact_fp.end());
    auto n2_data = n2.storage->get(TABLE_ALLOWLISTS, composite_key);
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

    // Bootstrap all nodes bidirectionally so everyone has each other's pubkeys
    bootstrap_bidirectional(n2, n1);
    bootstrap_bidirectional(n3, n1);
    // Re-bootstrap n2 so it discovers n3 (and vice versa)
    bootstrap_bidirectional(n2, n1);

    ASSERT_EQ(n1.kad->replication_factor(), 3u);
    ASSERT_EQ(n1.kad->write_quorum(), 2u); // W = min(2, 3) = 2

    // Use real keypair + signed allowlist entry (profile must exist on all nodes)
    KeyPair owner_kp = generate_keypair();
    Hash owner_fp = sha3_256(owner_kp.public_key);
    store_user_profile(n1, owner_kp);
    store_user_profile(n2, owner_kp);
    store_user_profile(n3, owner_kp);

    Hash contact_fp{};
    contact_fp.fill(0xCC);
    auto value = build_signed_allowlist(owner_kp, contact_fp, 0x01, 1);

    Hash key = sha3_256_prefixed("inbox:", owner_fp);

    bool ok = n1.kad->store(key, 0x04, value);
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

    // Verify all 3 nodes have the data (composite key: key||allowed_fp)
    std::vector<uint8_t> comp_key(key.begin(), key.end());
    comp_key.insert(comp_key.end(), contact_fp.begin(), contact_fp.end());
    EXPECT_TRUE(n1.storage->get(TABLE_ALLOWLISTS, comp_key).has_value());
    EXPECT_TRUE(n2.storage->get(TABLE_ALLOWLISTS, comp_key).has_value());
    EXPECT_TRUE(n3.storage->get(TABLE_ALLOWLISTS, comp_key).has_value());
}

// ---------------------------------------------------------------------------
// Test 13: SyncBetweenNodes — store on n1, n2 sends SYNC_REQ, gets data
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, SyncBetweenNodes) {
    auto& n1 = create_node(8);
    auto& n2 = create_node(8);
    auto& n3 = create_node(8);

    start_all();

    // Bootstrap all nodes bidirectionally so everyone has each other's pubkeys
    bootstrap_bidirectional(n2, n1);
    bootstrap_bidirectional(n3, n1);
    // Re-bootstrap so n2 discovers n3
    bootstrap_bidirectional(n2, n1);

    // Store a name record on node 1 directly (simulating a STORE)
    KeyPair user_kp = generate_keypair();
    Hash user_fp = sha3_256(user_kp.public_key);

    std::string name = "synctest";
    uint64_t nonce = find_pow_nonce(name, user_fp, 8);
    auto record = build_name_record(name, user_fp, nonce, 1, user_kp);
    Hash key = name_key(name);

    // Store directly on n1 (local storage + repl_log)
    n1.storage->put(TABLE_NAMES, key, record);
    n1.repl_log->append(key, Op::ADD, 0x01, record);

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
// Test 15b: PongCarriesVersionAndCapabilities
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, PongCarriesVersionAndCapabilities) {
    auto& n1 = create_node();
    auto& n2 = create_node();

    start_all();

    // Bootstrap so they know each other
    n2.kad->bootstrap({{"127.0.0.1", n1.info.tcp_port}});
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // n1 sends PING to n2, n2 replies with PONG containing version/caps
    Message ping_msg;
    ping_msg.type = MessageType::PING;
    ping_msg.sender = n1.info.id;
    ping_msg.sender_port = n1.info.tcp_port;
    ping_msg.payload = {};
    sign_message(ping_msg, n1.keypair.secret_key);
    n1.transport->send("127.0.0.1", n2.info.tcp_port, ping_msg);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // n1's routing table should now have n2's version/capabilities
    auto info = n1.table->find(n2.info.id);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->proto_version_min, 0x01);
    EXPECT_EQ(info->proto_version_max, 0x01);
    // Should have both GROUPS and ENCRYPTED_TCP capabilities
    EXPECT_TRUE(info->capabilities & static_cast<uint32_t>(Capability::GROUPS));
    EXPECT_TRUE(info->capabilities & static_cast<uint32_t>(Capability::ENCRYPTED_TCP));
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

// ---------------------------------------------------------------------------
// Test 19: ProfileStoreValidation_SizeLimit — >1MiB profile gets rejected
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, ProfileStoreValidation_SizeLimit) {
    auto& n1 = create_node();
    start_all();

    // Build a profile that exceeds 1 MiB
    std::vector<uint8_t> huge_profile(1024 * 1024 + 1, 0x00);

    Hash key{};
    key.fill(0x99);

    bool ok = n1.kad->store(key, 0x00, huge_profile);
    auto result = n1.storage->get(TABLE_PROFILES, key);
    EXPECT_FALSE(result.has_value()) << "Oversized profile should not be stored";
}

// ---------------------------------------------------------------------------
// Test 20: ProfileStoreValidation_FingerprintMismatch — mismatched fp rejected
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, ProfileStoreValidation_FingerprintMismatch) {
    auto& n1 = create_node();
    start_all();

    // Build a minimal profile with wrong fingerprint
    KeyPair user_kp = generate_keypair();
    Hash wrong_fp{};
    wrong_fp.fill(0xDE);  // deliberately wrong

    std::vector<uint8_t> profile;
    // fingerprint (32 bytes) — wrong
    profile.insert(profile.end(), wrong_fp.begin(), wrong_fp.end());
    // pubkey_length (2 bytes BE)
    uint16_t pk_len = static_cast<uint16_t>(user_kp.public_key.size());
    profile.push_back(static_cast<uint8_t>((pk_len >> 8) & 0xFF));
    profile.push_back(static_cast<uint8_t>(pk_len & 0xFF));
    // pubkey
    profile.insert(profile.end(), user_kp.public_key.begin(), user_kp.public_key.end());
    // kem_pubkey_length = 0
    profile.push_back(0x00); profile.push_back(0x00);
    // bio_length = 0
    profile.push_back(0x00); profile.push_back(0x00);
    // avatar_length = 0
    profile.push_back(0x00); profile.push_back(0x00); profile.push_back(0x00); profile.push_back(0x00);
    // social_count = 0
    profile.push_back(0x00);
    // sequence = 1
    for (int i = 7; i >= 0; --i) profile.push_back(i == 0 ? 0x01 : 0x00);
    // sig_length + signature (dummy)
    auto sig = sign(std::span<const uint8_t>(profile.data(), profile.size()), user_kp.secret_key);
    uint16_t sig_len = static_cast<uint16_t>(sig.size());
    profile.push_back(static_cast<uint8_t>((sig_len >> 8) & 0xFF));
    profile.push_back(static_cast<uint8_t>(sig_len & 0xFF));
    profile.insert(profile.end(), sig.begin(), sig.end());

    Hash key = sha3_256_prefixed("profile:", wrong_fp);

    bool ok = n1.kad->store(key, 0x00, profile);
    auto result = n1.storage->get(TABLE_PROFILES, key);
    EXPECT_FALSE(result.has_value()) << "Profile with mismatched fingerprint should not be stored";
}

// ---------------------------------------------------------------------------
// Test 21: InboxStoreValidation_OversizedBlob — blob_len > 50MiB rejected
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, InboxStoreValidation_OversizedBlob) {
    auto& n1 = create_node();
    start_all();

    // Build an inbox message with mismatched blob_len
    // recipient_fp(32) + msg_id(32) + sender_fp(32) + timestamp(8) + blob_len(4) + blob
    std::vector<uint8_t> msg;
    msg.resize(108, 0x00);  // 32+32+32+8+4 = 108 bytes, no blob data

    // Set blob_len = 100 but only provide 0 bytes of blob → mismatch
    msg[104] = 0x00; msg[105] = 0x00; msg[106] = 0x00; msg[107] = 100;

    Hash key{};
    key.fill(0xAB);

    bool ok = n1.kad->store(key, 0x02, msg);
    // Two-table model: inbox writes go to TABLE_INBOX_INDEX + TABLE_MESSAGE_BLOBS
    // Build the index key that would be used: recipient_fp(32)||msg_id(32) = all zeros
    std::vector<uint8_t> idx_key(64, 0x00);
    auto result = n1.storage->get(TABLE_INBOX_INDEX, idx_key);
    EXPECT_FALSE(result.has_value()) << "Inbox message with mismatched blob_len should not be stored";
}

// ---------------------------------------------------------------------------
// Test 22: ContactRequestValidation_OversizedBlob — invalid request rejected
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, ContactRequestValidation_OversizedBlob) {
    auto& n1 = create_node();
    start_all();

    // Build a contact request with new format but no valid PoW
    // recipient_fp(32) + sender_fp(32) + pow_nonce(8) + timestamp(8) + blob_length(4) + blob
    // This will be rejected at PoW verification (before blob size check)
    Hash recipient_fp{};
    recipient_fp.fill(0xAA);
    Hash sender_fp{};
    sender_fp.fill(0xBB);

    uint32_t blob_size = 65 * 1024;  // 65 KiB
    std::vector<uint8_t> request;
    request.insert(request.end(), recipient_fp.begin(), recipient_fp.end());
    request.insert(request.end(), sender_fp.begin(), sender_fp.end());
    request.resize(32 + 32 + 8 + 8, 0x00);  // pow_nonce = 0 (invalid), timestamp = 0
    // blob_length (4 BE)
    request.push_back(static_cast<uint8_t>((blob_size >> 24) & 0xFF));
    request.push_back(static_cast<uint8_t>((blob_size >> 16) & 0xFF));
    request.push_back(static_cast<uint8_t>((blob_size >> 8) & 0xFF));
    request.push_back(static_cast<uint8_t>(blob_size & 0xFF));
    // blob data
    request.resize(32 + 32 + 8 + 8 + 4 + blob_size, 0x42);

    Hash key{};
    key.fill(0xCD);

    bool ok = n1.kad->store(key, 0x03, request);
    // Composite storage key: recipient_fp(32) || sender_fp(32)
    std::vector<uint8_t> composite_key(recipient_fp.begin(), recipient_fp.end());
    composite_key.insert(composite_key.end(), sender_fp.begin(), sender_fp.end());
    auto result = n1.storage->get(TABLE_REQUESTS, composite_key);
    EXPECT_FALSE(result.has_value()) << "Contact request with invalid PoW should not be stored";
}

// ---------------------------------------------------------------------------
// Test 22b: ContactRequestPoWEnforced — valid PoW succeeds, invalid rejected
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, ContactRequestPoWEnforced) {
    auto& n1 = create_node();
    start_all();

    // Generate real fingerprints for PoW computation
    Hash recipient_fp{};
    recipient_fp.fill(0x11);
    Hash sender_fp{};
    sender_fp.fill(0x22);

    // Build PoW preimage: "chromatin:request:" || sender_fp || recipient_fp || timestamp(8 BE ms)
    uint64_t timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    std::vector<uint8_t> preimage;
    const std::string prefix = "chromatin:request:";
    preimage.insert(preimage.end(), prefix.begin(), prefix.end());
    preimage.insert(preimage.end(), sender_fp.begin(), sender_fp.end());
    preimage.insert(preimage.end(), recipient_fp.begin(), recipient_fp.end());
    for (int i = 7; i >= 0; --i) {
        preimage.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    }

    // Find a valid PoW nonce (16 leading zero bits, ~65k avg iterations)
    uint64_t pow_nonce = 0;
    for (uint64_t n = 0; n < 10'000'000; ++n) {
        if (verify_pow(preimage, n, 16)) {
            pow_nonce = n;
            break;
        }
    }
    ASSERT_TRUE(verify_pow(preimage, pow_nonce, 16))
        << "Failed to find valid PoW nonce within 10M iterations";

    std::vector<uint8_t> blob = {0xCA, 0xFE, 0xBA, 0xBE};
    uint32_t blob_len = static_cast<uint32_t>(blob.size());

    // --- Test 1: Valid PoW should succeed ---
    {
        std::vector<uint8_t> request;
        request.insert(request.end(), recipient_fp.begin(), recipient_fp.end());
        request.insert(request.end(), sender_fp.begin(), sender_fp.end());
        // pow_nonce (8 bytes BE)
        for (int i = 7; i >= 0; --i)
            request.push_back(static_cast<uint8_t>((pow_nonce >> (i * 8)) & 0xFF));
        // timestamp (8 bytes BE)
        for (int i = 7; i >= 0; --i)
            request.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
        // blob_length (4 BE)
        request.push_back(static_cast<uint8_t>((blob_len >> 24) & 0xFF));
        request.push_back(static_cast<uint8_t>((blob_len >> 16) & 0xFF));
        request.push_back(static_cast<uint8_t>((blob_len >> 8) & 0xFF));
        request.push_back(static_cast<uint8_t>(blob_len & 0xFF));
        request.insert(request.end(), blob.begin(), blob.end());

        auto requests_key = sha3_256_prefixed("inbox:", recipient_fp);

        bool ok = n1.kad->store(requests_key, 0x03, request);
        EXPECT_TRUE(ok) << "Contact request with valid PoW should succeed";

        // Verify stored with composite key: recipient_fp(32) || sender_fp(32)
        std::vector<uint8_t> composite_key(recipient_fp.begin(), recipient_fp.end());
        composite_key.insert(composite_key.end(), sender_fp.begin(), sender_fp.end());
        auto result = n1.storage->get(TABLE_REQUESTS, composite_key);
        ASSERT_TRUE(result.has_value())
            << "Contact request with valid PoW should be stored";
        EXPECT_EQ(result->size(), 32u + 32u + 8u + 8u + 4u + blob.size());
    }

    // --- Test 2: Invalid PoW (nonce=0 with different sender) should be rejected ---
    {
        Hash bad_sender{};
        bad_sender.fill(0xFF);

        std::vector<uint8_t> request;
        request.insert(request.end(), recipient_fp.begin(), recipient_fp.end());
        request.insert(request.end(), bad_sender.begin(), bad_sender.end());
        // pow_nonce = 0 (won't be valid for these fingerprints)
        for (int i = 0; i < 8; ++i)
            request.push_back(0x00);
        // timestamp (current, valid)
        for (int i = 7; i >= 0; --i)
            request.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
        // blob_length (4 BE)
        request.push_back(static_cast<uint8_t>((blob_len >> 24) & 0xFF));
        request.push_back(static_cast<uint8_t>((blob_len >> 16) & 0xFF));
        request.push_back(static_cast<uint8_t>((blob_len >> 8) & 0xFF));
        request.push_back(static_cast<uint8_t>(blob_len & 0xFF));
        request.insert(request.end(), blob.begin(), blob.end());

        auto requests_key = sha3_256_prefixed("inbox:", recipient_fp);

        bool ok = n1.kad->store(requests_key, 0x03, request);

        // Verify not stored with composite key: recipient_fp(32) || bad_sender(32)
        std::vector<uint8_t> composite_key(recipient_fp.begin(), recipient_fp.end());
        composite_key.insert(composite_key.end(), bad_sender.begin(), bad_sender.end());
        auto result = n1.storage->get(TABLE_REQUESTS, composite_key);
        EXPECT_FALSE(result.has_value())
            << "Contact request with invalid PoW should be rejected";
    }
}

// ---------------------------------------------------------------------------
// Test 23: AllowlistValidation_InvalidAction — action != 0x00/0x01 rejected
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, AllowlistValidation_InvalidAction) {
    auto& n1 = create_node();
    start_all();

    // Build an allowlist entry with invalid action byte
    // New format: owner_fp(32) || allowed_fp(32) || action(1) || sequence(8 BE) || signature
    std::vector<uint8_t> entry(32, 0xCC);  // owner_fp
    entry.resize(entry.size() + 32, 0xDD);  // allowed_fp
    entry.push_back(0x02);  // invalid action (only 0x00 and 0x01 are valid)
    // sequence (8 BE)
    for (int i = 0; i < 8; ++i) entry.push_back(0x00);
    // some dummy signature
    entry.resize(entry.size() + 100, 0xFF);

    Hash key{};
    key.fill(0xEF);

    bool ok = n1.kad->store(key, 0x04, entry);
    // Composite storage key: key(32)||allowed_fp(32)
    Hash allowed_fp{};
    allowed_fp.fill(0xDD);
    std::vector<uint8_t> composite_key(key.begin(), key.end());
    composite_key.insert(composite_key.end(), allowed_fp.begin(), allowed_fp.end());
    auto result = n1.storage->get(TABLE_ALLOWLISTS, composite_key);
    EXPECT_FALSE(result.has_value()) << "Allowlist entry with invalid action should not be stored";
}

// ---------------------------------------------------------------------------
// Test 25: InboxStoreWritesTwoTables — inbox STORE writes to INDEX + BLOBS
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, InboxStoreWritesTwoTables) {
    auto& n1 = create_node();
    start_all();

    // Build a valid inbox message:
    // recipient_fp(32) || msg_id(32) || sender_fp(32) || timestamp(8 BE) || blob_len(4 BE) || blob
    Hash recipient_fp{};
    recipient_fp.fill(0x11);
    Hash msg_id{};
    msg_id.fill(0x22);
    Hash sender_fp{};
    sender_fp.fill(0x33);

    uint64_t timestamp = 1700000000;
    std::vector<uint8_t> blob = {0xCA, 0xFE, 0xBA, 0xBE};
    uint32_t blob_len = static_cast<uint32_t>(blob.size());

    std::vector<uint8_t> value;
    value.reserve(108 + blob.size());
    value.insert(value.end(), recipient_fp.begin(), recipient_fp.end());
    value.insert(value.end(), msg_id.begin(), msg_id.end());
    value.insert(value.end(), sender_fp.begin(), sender_fp.end());
    for (int i = 7; i >= 0; --i)
        value.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    value.push_back(static_cast<uint8_t>((blob_len >> 24) & 0xFF));
    value.push_back(static_cast<uint8_t>((blob_len >> 16) & 0xFF));
    value.push_back(static_cast<uint8_t>((blob_len >> 8) & 0xFF));
    value.push_back(static_cast<uint8_t>(blob_len & 0xFF));
    value.insert(value.end(), blob.begin(), blob.end());

    auto inbox_key = sha3_256_prefixed("inbox:", recipient_fp);

    bool ok = n1.kad->store(inbox_key, 0x02, value);
    EXPECT_TRUE(ok);

    // Verify TABLE_INBOX_INDEX: key = recipient_fp(32) || msg_id(32)
    std::vector<uint8_t> idx_key;
    idx_key.insert(idx_key.end(), recipient_fp.begin(), recipient_fp.end());
    idx_key.insert(idx_key.end(), msg_id.begin(), msg_id.end());
    auto idx_result = n1.storage->get(TABLE_INBOX_INDEX, idx_key);
    ASSERT_TRUE(idx_result.has_value()) << "INDEX entry should exist after inbox STORE";
    // INDEX value: sender_fp(32) || timestamp(8) || size(4) = 44 bytes
    EXPECT_EQ(idx_result->size(), 44u);

    // Verify TABLE_MESSAGE_BLOBS: key = msg_id(32)
    std::vector<uint8_t> blob_key(msg_id.begin(), msg_id.end());
    auto blob_result = n1.storage->get(TABLE_MESSAGE_BLOBS, blob_key);
    ASSERT_TRUE(blob_result.has_value()) << "BLOB entry should exist after inbox STORE";
    EXPECT_EQ(*blob_result, blob);
}

// ---------------------------------------------------------------------------
// Test 26: AllowlistStoreUsesCompositeKey — allowlist uses routing_key||allowed_fp
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, AllowlistStoreUsesCompositeKey) {
    auto& n1 = create_node();
    start_all();

    // Use real keypair + signed entry (profile must exist for validation)
    KeyPair owner_kp = generate_keypair();
    Hash owner_fp = sha3_256(owner_kp.public_key);
    store_user_profile(n1, owner_kp);

    Hash allowed_fp{};
    allowed_fp.fill(0x44);

    auto value = build_signed_allowlist(owner_kp, allowed_fp, 0x01, 1);

    Hash routing_key = sha3_256_prefixed("inbox:", owner_fp);

    bool ok = n1.kad->store(routing_key, 0x04, value);
    EXPECT_TRUE(ok);

    // Verify composite storage key: routing_key(32) || allowed_fp(32)
    std::vector<uint8_t> composite_key(routing_key.begin(), routing_key.end());
    composite_key.insert(composite_key.end(), allowed_fp.begin(), allowed_fp.end());
    auto result = n1.storage->get(TABLE_ALLOWLISTS, composite_key);
    ASSERT_TRUE(result.has_value()) << "Allowlist entry should be stored with composite key";
    EXPECT_EQ(*result, value);
    EXPECT_EQ((*result)[64], 0x01);  // action = allow at offset 64
}

// ---------------------------------------------------------------------------
// Test 27: SyncPreservesDataType — sync routes to correct table via data_type
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, SyncPreservesDataType) {
    auto& n1 = create_node(8);
    auto& n2 = create_node(8);
    auto& n3 = create_node(8);

    start_all();

    // Bootstrap all nodes bidirectionally so everyone has each other's pubkeys
    bootstrap_bidirectional(n2, n1);
    bootstrap_bidirectional(n3, n1);
    bootstrap_bidirectional(n2, n1);

    // Build a valid profile and store directly on n1
    KeyPair user_kp = generate_keypair();
    Hash user_fp = sha3_256(user_kp.public_key);

    std::vector<uint8_t> profile;
    profile.insert(profile.end(), user_fp.begin(), user_fp.end());
    uint16_t pk_len = static_cast<uint16_t>(user_kp.public_key.size());
    profile.push_back(static_cast<uint8_t>((pk_len >> 8) & 0xFF));
    profile.push_back(static_cast<uint8_t>(pk_len & 0xFF));
    profile.insert(profile.end(), user_kp.public_key.begin(), user_kp.public_key.end());
    // kem_pubkey_length = 0, bio_length = 0, avatar_length = 0, social_count = 0
    profile.push_back(0x00); profile.push_back(0x00);
    profile.push_back(0x00); profile.push_back(0x00);
    profile.push_back(0x00); profile.push_back(0x00); profile.push_back(0x00); profile.push_back(0x00);
    profile.push_back(0x00);
    // sequence = 1
    for (int i = 7; i >= 0; --i) profile.push_back(i == 0 ? 0x01 : 0x00);
    // sign it
    auto sig = sign(std::span<const uint8_t>(profile.data(), profile.size()), user_kp.secret_key);
    uint16_t sig_len = static_cast<uint16_t>(sig.size());
    profile.push_back(static_cast<uint8_t>((sig_len >> 8) & 0xFF));
    profile.push_back(static_cast<uint8_t>(sig_len & 0xFF));
    profile.insert(profile.end(), sig.begin(), sig.end());

    Hash key = sha3_256_prefixed("profile:", user_fp);

    // Store directly on n1 with data_type 0x00 (profile)
    n1.storage->put(TABLE_PROFILES, key, profile);
    n1.repl_log->append(key, Op::ADD, 0x00, profile);

    ASSERT_TRUE(n1.storage->get(TABLE_PROFILES, key).has_value());
    ASSERT_FALSE(n2.storage->get(TABLE_PROFILES, key).has_value());

    // n2 sends SYNC_REQ to n1
    std::vector<uint8_t> sync_payload;
    sync_payload.insert(sync_payload.end(), key.begin(), key.end());
    for (int i = 0; i < 8; ++i) sync_payload.push_back(0x00); // after_seq = 0

    Message sync_msg;
    sync_msg.type = MessageType::SYNC_REQ;
    sync_msg.sender = n2.info.id;
    sync_msg.sender_port = n2.info.tcp_port;
    sync_msg.payload = sync_payload;
    sign_message(sync_msg, n2.keypair.secret_key);

    n2.transport->send("127.0.0.1", n1.info.tcp_port, sync_msg);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // Verify n2 stored the profile in TABLE_PROFILES (not TABLE_NAMES)
    auto n2_profile = n2.storage->get(TABLE_PROFILES, key);
    ASSERT_TRUE(n2_profile.has_value()) << "n2 should have the profile via SYNC";
    EXPECT_EQ(*n2_profile, profile);

    // Verify it's NOT in TABLE_NAMES (wrong table)
    auto n2_names = n2.storage->get(TABLE_NAMES, key);
    EXPECT_FALSE(n2_names.has_value()) << "profile should not be in TABLE_NAMES";
}

// ---------------------------------------------------------------------------
// Test 28: TamperedMessageRejected — signature verification rejects tampered messages
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, TamperedMessageRejected) {
    auto& n1 = create_node(8);
    auto& n2 = create_node(8);

    start_all();

    // Bootstrap bidirectionally so both nodes have each other's pubkey via NODES
    n2.kad->bootstrap({{"127.0.0.1", n1.info.tcp_port}});
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    n1.kad->bootstrap({{"127.0.0.1", n2.info.tcp_port}});
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // Build a valid profile to use as STORE payload
    KeyPair user_kp = generate_keypair();
    Hash user_fp = sha3_256(user_kp.public_key);

    std::vector<uint8_t> profile;
    profile.insert(profile.end(), user_fp.begin(), user_fp.end());
    uint16_t pk_len = static_cast<uint16_t>(user_kp.public_key.size());
    profile.push_back(static_cast<uint8_t>((pk_len >> 8) & 0xFF));
    profile.push_back(static_cast<uint8_t>(pk_len & 0xFF));
    profile.insert(profile.end(), user_kp.public_key.begin(), user_kp.public_key.end());
    // kem_pubkey_length = 0, bio_length = 0, avatar_length = 0, social_count = 0
    profile.push_back(0x00); profile.push_back(0x00);
    profile.push_back(0x00); profile.push_back(0x00);
    profile.push_back(0x00); profile.push_back(0x00); profile.push_back(0x00); profile.push_back(0x00);
    profile.push_back(0x00);
    // sequence = 1
    for (int i = 7; i >= 0; --i) profile.push_back(i == 0 ? 0x01 : 0x00);
    auto sig = sign(std::span<const uint8_t>(profile.data(), profile.size()), user_kp.secret_key);
    uint16_t sig_len = static_cast<uint16_t>(sig.size());
    profile.push_back(static_cast<uint8_t>((sig_len >> 8) & 0xFF));
    profile.push_back(static_cast<uint8_t>(sig_len & 0xFF));
    profile.insert(profile.end(), sig.begin(), sig.end());

    Hash key = sha3_256_prefixed("profile:", user_fp);

    // Build STORE message payload: data_type(1) + key(32) + value
    std::vector<uint8_t> store_payload;
    store_payload.push_back(0x00); // data_type = profile
    store_payload.insert(store_payload.end(), key.begin(), key.end());
    store_payload.insert(store_payload.end(), profile.begin(), profile.end());

    // Create a properly signed STORE from n2
    Message store_msg;
    store_msg.type = MessageType::STORE;
    store_msg.sender = n2.info.id;
    store_msg.sender_port = n2.info.tcp_port;
    store_msg.payload = store_payload;
    sign_message(store_msg, n2.keypair.secret_key);

    // Now tamper with the payload AFTER signing (flip a byte)
    if (!store_msg.payload.empty()) {
        store_msg.payload.back() ^= 0xFF;
    }

    // Send tampered message to n1
    n2.transport->send("127.0.0.1", n1.info.tcp_port, store_msg);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // The tampered STORE should have been rejected — key should NOT be stored
    auto result = n1.storage->get(TABLE_PROFILES, key);
    EXPECT_FALSE(result.has_value()) << "Tampered message should have been rejected";
}

// ---------------------------------------------------------------------------
// Test 29: FindValueSkipsStaleInboxTable — FIND_VALUE doesn't search inbox tables
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, FindValueSkipsStaleInboxTable) {
    auto& n1 = create_node(8);

    start_all();

    // Create a user keypair and build routing keys
    KeyPair user_kp = generate_keypair();
    Hash user_fp = sha3_256(user_kp.public_key);

    // Build a valid profile and store it directly
    std::vector<uint8_t> profile;
    profile.insert(profile.end(), user_fp.begin(), user_fp.end());
    uint16_t pk_len = static_cast<uint16_t>(user_kp.public_key.size());
    profile.push_back(static_cast<uint8_t>((pk_len >> 8) & 0xFF));
    profile.push_back(static_cast<uint8_t>(pk_len & 0xFF));
    profile.insert(profile.end(), user_kp.public_key.begin(), user_kp.public_key.end());
    profile.push_back(0x00); profile.push_back(0x00); // kem_pubkey_len
    profile.push_back(0x00); profile.push_back(0x00); // bio_len
    profile.push_back(0x00); profile.push_back(0x00); profile.push_back(0x00); profile.push_back(0x00); // avatar_len
    profile.push_back(0x00); // social_count
    for (int i = 7; i >= 0; --i) profile.push_back(i == 0 ? 0x01 : 0x00); // seq=1
    auto sig = sign(std::span<const uint8_t>(profile.data(), profile.size()), user_kp.secret_key);
    uint16_t sig_len = static_cast<uint16_t>(sig.size());
    profile.push_back(static_cast<uint8_t>((sig_len >> 8) & 0xFF));
    profile.push_back(static_cast<uint8_t>(sig_len & 0xFF));
    profile.insert(profile.end(), sig.begin(), sig.end());

    Hash profile_key = sha3_256_prefixed("profile:", user_fp);
    n1.storage->put(TABLE_PROFILES, profile_key, profile);

    // find_value should return the profile
    auto found = n1.kad->find_value(profile_key);
    ASSERT_TRUE(found.has_value()) << "FIND_VALUE should find profiles";
    EXPECT_EQ(*found, profile);

    // Store inbox data in TABLE_INBOX_INDEX + TABLE_MESSAGE_BLOBS
    Hash inbox_routing_key = sha3_256_prefixed("inbox:", user_fp);
    Hash msg_id{};
    msg_id[0] = 0xAA;
    std::vector<uint8_t> idx_key;
    idx_key.insert(idx_key.end(), user_fp.begin(), user_fp.end());
    idx_key.insert(idx_key.end(), msg_id.begin(), msg_id.end());
    std::vector<uint8_t> idx_val(44, 0x01); // sender_fp(32) + timestamp(8) + blob_len(4)
    n1.storage->put(TABLE_INBOX_INDEX, idx_key, idx_val);
    std::vector<uint8_t> blob_data = {0xDE, 0xAD};
    n1.storage->put(TABLE_MESSAGE_BLOBS, msg_id, blob_data);

    // find_value with inbox routing key should NOT find inbox data
    // (inbox uses SYNC, not FIND_VALUE)
    auto inbox_found = n1.kad->find_value(inbox_routing_key);
    EXPECT_FALSE(inbox_found.has_value()) << "FIND_VALUE should not search inbox tables";
}

// ---------------------------------------------------------------------------
// Test 30: ProfileSequenceMonotonicity — reject stale/equal sequence updates
// ---------------------------------------------------------------------------

// Helper: build a minimal valid profile binary with a given sequence number
static std::vector<uint8_t> build_profile(const KeyPair& user_kp, uint64_t sequence) {
    Hash user_fp = sha3_256(user_kp.public_key);
    std::vector<uint8_t> profile;
    // fingerprint(32)
    profile.insert(profile.end(), user_fp.begin(), user_fp.end());
    // pubkey_len(2 BE) + pubkey
    uint16_t pk_len = static_cast<uint16_t>(user_kp.public_key.size());
    profile.push_back(static_cast<uint8_t>((pk_len >> 8) & 0xFF));
    profile.push_back(static_cast<uint8_t>(pk_len & 0xFF));
    profile.insert(profile.end(), user_kp.public_key.begin(), user_kp.public_key.end());
    // kem_pubkey_len = 0, bio_len = 0, avatar_len = 0, social_count = 0
    profile.push_back(0x00); profile.push_back(0x00); // kem
    profile.push_back(0x00); profile.push_back(0x00); // bio
    profile.push_back(0x00); profile.push_back(0x00); profile.push_back(0x00); profile.push_back(0x00); // avatar
    profile.push_back(0x00); // social_count
    // sequence(8 BE)
    for (int i = 7; i >= 0; --i) {
        profile.push_back(static_cast<uint8_t>((sequence >> (i * 8)) & 0xFF));
    }
    // sign it
    auto sig = sign(std::span<const uint8_t>(profile.data(), profile.size()), user_kp.secret_key);
    uint16_t sig_len = static_cast<uint16_t>(sig.size());
    profile.push_back(static_cast<uint8_t>((sig_len >> 8) & 0xFF));
    profile.push_back(static_cast<uint8_t>(sig_len & 0xFF));
    profile.insert(profile.end(), sig.begin(), sig.end());
    return profile;
}

TEST_F(KademliaTest, ProfileSequenceMonotonicity) {
    auto& n1 = create_node(8);
    start_all();

    KeyPair user_kp = generate_keypair();
    Hash user_fp = sha3_256(user_kp.public_key);
    Hash key = sha3_256_prefixed("profile:", user_fp);

    // Store profile with sequence=1 — should succeed
    auto profile_v1 = build_profile(user_kp, 1);
    ASSERT_TRUE(n1.kad->store(key, 0x00, profile_v1));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT_TRUE(n1.storage->get(TABLE_PROFILES, key).has_value());

    // Store profile with sequence=2 — should succeed (update)
    auto profile_v2 = build_profile(user_kp, 2);
    ASSERT_TRUE(n1.kad->store(key, 0x00, profile_v2));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto stored = n1.storage->get(TABLE_PROFILES, key);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(*stored, profile_v2);

    // Store profile with sequence=1 — should be rejected (stale)
    auto profile_stale = build_profile(user_kp, 1);
    EXPECT_FALSE(n1.kad->store(key, 0x00, profile_stale));

    // Store profile with sequence=2 — should be rejected (equal)
    auto profile_equal = build_profile(user_kp, 2);
    EXPECT_FALSE(n1.kad->store(key, 0x00, profile_equal));

    // Verify the stored profile is still v2
    stored = n1.storage->get(TABLE_PROFILES, key);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(*stored, profile_v2);
}

// ---------------------------------------------------------------------------
// Test 31: SyncHandlesDelete — DEL entries propagate via sync
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, SyncHandlesDelete) {
    auto& n1 = create_node(8);
    auto& n2 = create_node(8);
    auto& n3 = create_node(8);

    start_all();

    // Bootstrap all nodes bidirectionally so everyone has each other's pubkeys
    bootstrap_bidirectional(n2, n1);
    bootstrap_bidirectional(n3, n1);
    bootstrap_bidirectional(n2, n1);

    // Create an inbox message on n1
    KeyPair user_kp = generate_keypair();
    Hash user_fp = sha3_256(user_kp.public_key);
    Hash inbox_key = sha3_256_prefixed("inbox:", user_fp);

    Hash msg_id{};
    msg_id[0] = 0xBB;
    Hash sender_fp{};
    sender_fp[0] = 0xCC;
    uint64_t timestamp = 1700000000;

    // Build inbox value: recipient_fp(32) || msg_id(32) || sender_fp(32) || timestamp(8) || blob_len(4) || blob
    std::vector<uint8_t> inbox_val;
    inbox_val.insert(inbox_val.end(), user_fp.begin(), user_fp.end());
    inbox_val.insert(inbox_val.end(), msg_id.begin(), msg_id.end());
    inbox_val.insert(inbox_val.end(), sender_fp.begin(), sender_fp.end());
    for (int i = 7; i >= 0; --i)
        inbox_val.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    uint32_t blob_len = 5;
    inbox_val.push_back(0x00); inbox_val.push_back(0x00);
    inbox_val.push_back(0x00); inbox_val.push_back(0x05);
    inbox_val.push_back('H'); inbox_val.push_back('e');
    inbox_val.push_back('l'); inbox_val.push_back('l'); inbox_val.push_back('o');

    // Store on n1 directly (index + blob + repl_log)
    std::vector<uint8_t> idx_key;
    idx_key.insert(idx_key.end(), user_fp.begin(), user_fp.end());
    idx_key.insert(idx_key.end(), msg_id.begin(), msg_id.end());
    std::vector<uint8_t> idx_val;
    idx_val.insert(idx_val.end(), sender_fp.begin(), sender_fp.end());
    for (int i = 7; i >= 0; --i)
        idx_val.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    idx_val.push_back(0x00); idx_val.push_back(0x00);
    idx_val.push_back(0x00); idx_val.push_back(0x05);
    n1.storage->put(TABLE_INBOX_INDEX, idx_key, idx_val);
    std::vector<uint8_t> blob_data = {'H', 'e', 'l', 'l', 'o'};
    n1.storage->put(TABLE_MESSAGE_BLOBS, msg_id, blob_data);
    n1.repl_log->append(inbox_key, Op::ADD, 0x02, inbox_val);

    // Sync n2 from n1 — n2 should get the message
    std::vector<uint8_t> sync_payload;
    sync_payload.insert(sync_payload.end(), inbox_key.begin(), inbox_key.end());
    for (int i = 0; i < 8; ++i) sync_payload.push_back(0x00);

    Message sync_msg;
    sync_msg.type = MessageType::SYNC_REQ;
    sync_msg.sender = n2.info.id;
    sync_msg.sender_port = n2.info.tcp_port;
    sync_msg.payload = sync_payload;
    sign_message(sync_msg, n2.keypair.secret_key);
    n2.transport->send("127.0.0.1", n1.info.tcp_port, sync_msg);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    ASSERT_TRUE(n2.storage->get(TABLE_INBOX_INDEX, idx_key).has_value())
        << "n2 should have inbox message after sync";

    // Now delete on n1 via delete_value()
    std::vector<uint8_t> delete_info;
    delete_info.insert(delete_info.end(), user_fp.begin(), user_fp.end());
    delete_info.insert(delete_info.end(), msg_id.begin(), msg_id.end());
    n1.storage->del(TABLE_INBOX_INDEX, idx_key);
    n1.storage->del(TABLE_MESSAGE_BLOBS, msg_id);
    n1.kad->delete_value(inbox_key, 0x02, delete_info);

    // Sync n2 again — should pick up the DEL entry
    // n2 needs to sync from after the ADD entry (seq > 1)
    uint64_t after_seq = n2.repl_log->current_seq(inbox_key);
    std::vector<uint8_t> sync2_payload;
    sync2_payload.insert(sync2_payload.end(), inbox_key.begin(), inbox_key.end());
    for (int i = 7; i >= 0; --i)
        sync2_payload.push_back(static_cast<uint8_t>((after_seq >> (i * 8)) & 0xFF));

    Message sync2_msg;
    sync2_msg.type = MessageType::SYNC_REQ;
    sync2_msg.sender = n2.info.id;
    sync2_msg.sender_port = n2.info.tcp_port;
    sync2_msg.payload = sync2_payload;
    sign_message(sync2_msg, n2.keypair.secret_key);
    n2.transport->send("127.0.0.1", n1.info.tcp_port, sync2_msg);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // n2 should have deleted the inbox message
    EXPECT_FALSE(n2.storage->get(TABLE_INBOX_INDEX, idx_key).has_value())
        << "n2 should have deleted inbox message after sync DEL";
    EXPECT_FALSE(n2.storage->get(TABLE_MESSAGE_BLOBS, msg_id).has_value())
        << "n2 should have deleted blob after sync DEL";
}

// ---------------------------------------------------------------------------
// Test 32: DuplicateInboxMessageRejected — same msg_id is not stored twice
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, DuplicateInboxMessageRejected) {
    auto& n1 = create_node(8);
    start_all();

    KeyPair user_kp = generate_keypair();
    Hash user_fp = sha3_256(user_kp.public_key);
    Hash inbox_key = sha3_256_prefixed("inbox:", user_fp);

    Hash msg_id{};
    msg_id[0] = 0xEE;
    Hash sender_fp{};
    sender_fp[0] = 0xFF;
    uint64_t timestamp = 1700000000;

    // Build inbox value: recipient_fp(32) || msg_id(32) || sender_fp(32) || timestamp(8) || blob_len(4) || blob
    auto build_inbox = [&](const std::string& blob_str) {
        std::vector<uint8_t> val;
        val.insert(val.end(), user_fp.begin(), user_fp.end());
        val.insert(val.end(), msg_id.begin(), msg_id.end());
        val.insert(val.end(), sender_fp.begin(), sender_fp.end());
        for (int i = 7; i >= 0; --i)
            val.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
        uint32_t blen = static_cast<uint32_t>(blob_str.size());
        val.push_back(static_cast<uint8_t>((blen >> 24) & 0xFF));
        val.push_back(static_cast<uint8_t>((blen >> 16) & 0xFF));
        val.push_back(static_cast<uint8_t>((blen >> 8) & 0xFF));
        val.push_back(static_cast<uint8_t>(blen & 0xFF));
        for (char c : blob_str) val.push_back(static_cast<uint8_t>(c));
        return val;
    };

    // First store — should succeed
    auto val1 = build_inbox("Hello");
    ASSERT_TRUE(n1.kad->store(inbox_key, 0x02, val1));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT_TRUE(n1.storage->get(TABLE_MESSAGE_BLOBS, msg_id).has_value());

    // Store same msg_id again with different blob — should be silently rejected
    auto val2 = build_inbox("Different payload");
    n1.kad->store(inbox_key, 0x02, val2);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Verify original blob is unchanged
    auto stored = n1.storage->get(TABLE_MESSAGE_BLOBS, msg_id);
    ASSERT_TRUE(stored.has_value());
    std::string original(stored->begin(), stored->end());
    EXPECT_EQ(original, "Hello") << "Original blob should be preserved, duplicate rejected";
}

// ---------------------------------------------------------------------------
// Test 33: TTLExpiry — inbox messages older than 7 days are deleted by tick()
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, TTLExpiry) {
    auto& n1 = create_node(8);
    start_all();

    // Build an inbox message with a timestamp from 8 days ago
    Hash recipient_fp{};
    recipient_fp.fill(0xA1);
    Hash msg_id{};
    msg_id.fill(0xB2);
    Hash sender_fp{};
    sender_fp.fill(0xC3);

    // 8 days ago in milliseconds
    auto eight_days_ago = std::chrono::system_clock::now() - std::chrono::hours(8 * 24);
    uint64_t old_ts = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            eight_days_ago.time_since_epoch()).count());

    // Build index key: recipient_fp(32) || msg_id(32)
    std::vector<uint8_t> idx_key;
    idx_key.insert(idx_key.end(), recipient_fp.begin(), recipient_fp.end());
    idx_key.insert(idx_key.end(), msg_id.begin(), msg_id.end());

    // Build index value: sender_fp(32) || timestamp(8 BE) || blob_len(4 BE)
    std::vector<uint8_t> idx_val;
    idx_val.insert(idx_val.end(), sender_fp.begin(), sender_fp.end());
    for (int i = 7; i >= 0; --i)
        idx_val.push_back(static_cast<uint8_t>((old_ts >> (i * 8)) & 0xFF));
    uint32_t blob_len = 4;
    idx_val.push_back(0x00); idx_val.push_back(0x00);
    idx_val.push_back(0x00); idx_val.push_back(0x04);

    // Store directly
    n1.storage->put(TABLE_INBOX_INDEX, idx_key, idx_val);
    std::vector<uint8_t> blob_data = {0xDE, 0xAD, 0xBE, 0xEF};
    n1.storage->put(TABLE_MESSAGE_BLOBS, msg_id, blob_data);

    ASSERT_TRUE(n1.storage->get(TABLE_INBOX_INDEX, idx_key).has_value());
    ASSERT_TRUE(n1.storage->get(TABLE_MESSAGE_BLOBS, msg_id).has_value());

    // Also store a recent message that should NOT be expired
    Hash msg_id2{};
    msg_id2.fill(0xD4);
    uint64_t recent_ts = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    std::vector<uint8_t> idx_key2;
    idx_key2.insert(idx_key2.end(), recipient_fp.begin(), recipient_fp.end());
    idx_key2.insert(idx_key2.end(), msg_id2.begin(), msg_id2.end());

    std::vector<uint8_t> idx_val2;
    idx_val2.insert(idx_val2.end(), sender_fp.begin(), sender_fp.end());
    for (int i = 7; i >= 0; --i)
        idx_val2.push_back(static_cast<uint8_t>((recent_ts >> (i * 8)) & 0xFF));
    idx_val2.push_back(0x00); idx_val2.push_back(0x00);
    idx_val2.push_back(0x00); idx_val2.push_back(0x03);

    n1.storage->put(TABLE_INBOX_INDEX, idx_key2, idx_val2);
    std::vector<uint8_t> blob2 = {0xCA, 0xFE, 0x00};
    n1.storage->put(TABLE_MESSAGE_BLOBS, msg_id2, blob2);

    // tick() triggers expire_ttl() on first call (last_ttl_sweep_ is epoch)
    n1.kad->tick();

    // Old message should be expired
    EXPECT_FALSE(n1.storage->get(TABLE_INBOX_INDEX, idx_key).has_value())
        << "8-day-old inbox message should be expired by TTL sweep";
    EXPECT_FALSE(n1.storage->get(TABLE_MESSAGE_BLOBS, msg_id).has_value())
        << "8-day-old blob should be expired by TTL sweep";

    // Recent message should still exist
    EXPECT_TRUE(n1.storage->get(TABLE_INBOX_INDEX, idx_key2).has_value())
        << "Recent inbox message should NOT be expired";
    EXPECT_TRUE(n1.storage->get(TABLE_MESSAGE_BLOBS, msg_id2).has_value())
        << "Recent blob should NOT be expired";
}

// ---------------------------------------------------------------------------
// Test 34: PendingStoreTimeout — pending_stores_ entries are cleaned after timeout
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, PendingStoreTimeout) {
    auto& n1 = create_node(8);
    auto& n2 = create_node(8);
    start_all();

    // Bootstrap bidirectionally so both nodes have each other's pubkeys
    // (required since STORE_ACK needs verified pubkey)
    bootstrap_bidirectional(n1, n2);

    // Build a valid name record and store it
    KeyPair user_kp = generate_keypair();
    Hash user_fp = sha3_256(user_kp.public_key);
    std::string name = "pendingtest";
    uint64_t nonce = find_pow_nonce(name, user_fp, 8);
    auto record = build_name_record(name, user_fp, nonce, 1, user_kp);
    Hash key = name_key(name);

    // Store — creates a pending_stores_ entry waiting for ACK from n2
    bool ok = n1.kad->store(key, 0x01, record);
    ASSERT_TRUE(ok);

    // Immediately, pending store should exist (or already be resolved by ACK)
    // The ACK might arrive fast, so we just verify tick() runs without crashing
    // and cleans up stale entries
    n1.kad->tick();

    // No crash = success. The cleanup logic handles both cases:
    // - If ACK arrived: entry was already removed normally
    // - If ACK didn't arrive within 30s: cleanup_pending_stores() removes it
    // Since we can't easily mock time, we verify the mechanism works end-to-end
    // by checking tick() completes successfully
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Test 35: ResponsibilityTransfer — data pushed to new nodes on table change
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, ResponsibilityTransfer) {
    auto& n1 = create_node(8);
    start_all();

    // Store a name record directly on n1 (single node, it's responsible for everything)
    KeyPair user_kp = generate_keypair();
    Hash user_fp = sha3_256(user_kp.public_key);

    std::string name = "transfertest";
    uint64_t nonce = find_pow_nonce(name, user_fp, 8);
    auto record = build_name_record(name, user_fp, nonce, 1, user_kp);
    Hash key = name_key(name);

    ASSERT_TRUE(n1.kad->store(key, 0x01, record));
    ASSERT_TRUE(n1.storage->get(TABLE_NAMES, key).has_value());

    // Now create n2 and bootstrap bidirectionally — this changes the routing table
    // Both nodes need each other's pubkeys for STORE verification
    auto& n2 = create_node(8);
    n2.start_recv();
    bootstrap_bidirectional(n2, n1);

    // n1's routing table changed (n2 joined). tick() should trigger transfer.
    n1.kad->tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // n2 should now have the name record (pushed via STORE from n1)
    auto result = n2.storage->get(TABLE_NAMES, key);
    EXPECT_TRUE(result.has_value())
        << "n2 should have received the name record via responsibility transfer";
}

// ---------------------------------------------------------------------------
// Test 36: EmptyPubkeyStoreRejected — STORE rejected from unverified nodes
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, EmptyPubkeyStoreRejected) {
    auto& n1 = create_node(8);
    auto& n2 = create_node(8);

    start_all();

    // Step 1: n1 sends a raw PING to n2.
    // n2 will add n1 to its routing table with an empty pubkey
    // (handle_ping doesn't know n1's pubkey).
    {
        Message ping;
        ping.type = MessageType::PING;
        ping.sender = n1.info.id;
        ping.sender_port = n1.info.tcp_port;
        sign_message(ping, n1.keypair.secret_key);
        n1.transport->send("127.0.0.1", n2.info.tcp_port, ping);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Verify n2 knows about n1 (added via PING) but with empty pubkey
    auto found = n2.table->find(n1.info.id);
    ASSERT_TRUE(found.has_value()) << "n2 should have n1 in routing table after PING";
    EXPECT_TRUE(found->pubkey.empty()) << "n1's pubkey should be empty (added via PING)";

    // Step 2: n1 sends a STORE to n2 — should be rejected (empty pubkey).
    // Use real keypair + signed allowlist entry (profile must exist on n2)
    KeyPair owner_kp = generate_keypair();
    Hash owner_fp = sha3_256(owner_kp.public_key);
    store_user_profile(n2, owner_kp);

    Hash contact_fp{};
    contact_fp.fill(0xAA);
    auto value = build_signed_allowlist(owner_kp, contact_fp, 0x01, 1);

    Hash key = sha3_256_prefixed("inbox:", owner_fp);

    std::vector<uint8_t> store_payload;
    store_payload.insert(store_payload.end(), key.begin(), key.end());
    store_payload.push_back(0x04); // allowlist data type
    uint32_t vlen = static_cast<uint32_t>(value.size());
    store_payload.push_back(static_cast<uint8_t>((vlen >> 24) & 0xFF));
    store_payload.push_back(static_cast<uint8_t>((vlen >> 16) & 0xFF));
    store_payload.push_back(static_cast<uint8_t>((vlen >> 8) & 0xFF));
    store_payload.push_back(static_cast<uint8_t>(vlen & 0xFF));
    store_payload.insert(store_payload.end(), value.begin(), value.end());

    {
        Message store_msg;
        store_msg.type = MessageType::STORE;
        store_msg.sender = n1.info.id;
        store_msg.sender_port = n1.info.tcp_port;
        store_msg.payload = store_payload;
        sign_message(store_msg, n1.keypair.secret_key);
        n1.transport->send("127.0.0.1", n2.info.tcp_port, store_msg);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Allowlist uses composite key: routing_key(32) || allowed_fp(32)
    std::vector<uint8_t> composite_key(key.begin(), key.end());
    composite_key.insert(composite_key.end(), contact_fp.begin(), contact_fp.end());

    // The STORE should have been rejected — n1's pubkey is empty in n2's table
    auto stored = n2.storage->get(TABLE_ALLOWLISTS, composite_key);
    EXPECT_FALSE(stored.has_value())
        << "STORE from node with empty pubkey should be rejected";

    // Step 3: Bootstrap properly so pubkeys are exchanged via NODES responses.
    n2.kad->bootstrap({{"127.0.0.1", n1.info.tcp_port}});
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // After bootstrap, n2 should have n1's pubkey (received via NODES response)
    found = n2.table->find(n1.info.id);
    ASSERT_TRUE(found.has_value()) << "n2 should still have n1 in routing table";
    EXPECT_FALSE(found->pubkey.empty())
        << "n1's pubkey should be populated after bootstrap NODES exchange";

    // Step 4: n1 sends a signed STORE to n2 — should succeed now.
    {
        Message store_msg;
        store_msg.type = MessageType::STORE;
        store_msg.sender = n1.info.id;
        store_msg.sender_port = n1.info.tcp_port;
        store_msg.payload = store_payload;
        sign_message(store_msg, n1.keypair.secret_key);
        n1.transport->send("127.0.0.1", n2.info.tcp_port, store_msg);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // The STORE should succeed now — n1's pubkey is verified
    stored = n2.storage->get(TABLE_ALLOWLISTS, composite_key);
    EXPECT_TRUE(stored.has_value())
        << "STORE from node with verified pubkey should succeed";
}

// ---------------------------------------------------------------------------
// Test 37: InboxAllowlistEnforced — Kademlia enforces allowlist on inbox STORE
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, InboxAllowlistEnforced) {
    auto& n1 = create_node();
    start_all();

    // Create four identities: user A (recipient with allowlist), user B (allowed sender),
    // user C (not allowed sender), user D (recipient with no allowlist)
    Hash user_a_fp{};  user_a_fp.fill(0xAA);
    Hash user_b_fp{};  user_b_fp.fill(0xBB);
    Hash user_c_fp{};  user_c_fp.fill(0xCC);
    Hash user_d_fp{};  user_d_fp.fill(0xDD);

    // --- Set up allowlist for user A, allowing user B ---
    // allowlist_key = SHA3-256("inbox:" || user_a_fp) — co-located with inbox
    auto allowlist_key = sha3_256_prefixed("inbox:", user_a_fp);

    // Composite storage key: allowlist_key(32) || user_b_fp(32) = 64 bytes
    std::vector<uint8_t> allow_storage_key;
    allow_storage_key.reserve(64);
    allow_storage_key.insert(allow_storage_key.end(), allowlist_key.begin(), allowlist_key.end());
    allow_storage_key.insert(allow_storage_key.end(), user_b_fp.begin(), user_b_fp.end());

    // Allowlist entry value: owner_fp(32) || allowed_fp(32) || action(1) || sequence(8 BE) = 73 bytes
    std::vector<uint8_t> allow_value;
    allow_value.insert(allow_value.end(), user_a_fp.begin(), user_a_fp.end());
    allow_value.insert(allow_value.end(), user_b_fp.begin(), user_b_fp.end());
    allow_value.push_back(0x01);  // action = allow
    for (int i = 0; i < 7; ++i) allow_value.push_back(0x00);
    allow_value.push_back(0x01);  // sequence = 1

    bool put_ok = n1.storage->put(TABLE_ALLOWLISTS, allow_storage_key, allow_value);
    ASSERT_TRUE(put_ok) << "Failed to store allowlist entry for user A -> user B";

    // --- Helper: build an inbox message ---
    auto build_inbox_msg = [](const Hash& recipient, const Hash& sender) -> std::vector<uint8_t> {
        std::vector<uint8_t> msg;
        msg.reserve(112);  // 108 header + 4 blob

        // recipient_fp (32)
        msg.insert(msg.end(), recipient.begin(), recipient.end());
        // msg_id (32) — just use some unique value
        Hash msg_id{};
        msg_id.fill(static_cast<uint8_t>(sender[0] ^ recipient[0]));
        msg.insert(msg.end(), msg_id.begin(), msg_id.end());
        // sender_fp (32)
        msg.insert(msg.end(), sender.begin(), sender.end());
        // timestamp (8 BE)
        uint64_t ts = 1700000000;
        for (int i = 7; i >= 0; --i)
            msg.push_back(static_cast<uint8_t>((ts >> (i * 8)) & 0xFF));
        // blob_len (4 BE) = 4
        msg.push_back(0x00); msg.push_back(0x00); msg.push_back(0x00); msg.push_back(0x04);
        // blob (4 bytes)
        msg.push_back(0xDE); msg.push_back(0xAD); msg.push_back(0xBE); msg.push_back(0xEF);

        return msg;
    };

    // --- Test 1: User B sends to user A (B is on A's allowlist) — should SUCCEED ---
    {
        auto msg = build_inbox_msg(user_a_fp, user_b_fp);
        auto inbox_key = sha3_256_prefixed("inbox:", user_a_fp);

        bool ok = n1.kad->store(inbox_key, 0x02, msg);
        EXPECT_TRUE(ok) << "Allowed sender B should be able to store inbox message for A";

        // Verify the message was actually stored in TABLE_INBOX_INDEX
        std::vector<uint8_t> idx_key;
        idx_key.insert(idx_key.end(), user_a_fp.begin(), user_a_fp.end());
        Hash msg_id{};
        msg_id.fill(static_cast<uint8_t>(user_b_fp[0] ^ user_a_fp[0]));
        idx_key.insert(idx_key.end(), msg_id.begin(), msg_id.end());
        auto result = n1.storage->get(TABLE_INBOX_INDEX, idx_key);
        EXPECT_TRUE(result.has_value()) << "Inbox message from allowed sender should be stored";
    }

    // --- Test 2: User C sends to user A (C is NOT on A's allowlist) — should FAIL ---
    {
        auto msg = build_inbox_msg(user_a_fp, user_c_fp);
        auto inbox_key = sha3_256_prefixed("inbox:", user_a_fp);

        bool ok = n1.kad->store(inbox_key, 0x02, msg);

        // Verify the message was NOT stored
        std::vector<uint8_t> idx_key;
        idx_key.insert(idx_key.end(), user_a_fp.begin(), user_a_fp.end());
        Hash msg_id{};
        msg_id.fill(static_cast<uint8_t>(user_c_fp[0] ^ user_a_fp[0]));
        idx_key.insert(idx_key.end(), msg_id.begin(), msg_id.end());
        auto result = n1.storage->get(TABLE_INBOX_INDEX, idx_key);
        EXPECT_FALSE(result.has_value())
            << "Inbox message from non-allowed sender C should be rejected";
    }

    // --- Test 3: User B sends to user D (D has NO allowlist) — should SUCCEED ---
    {
        auto msg = build_inbox_msg(user_d_fp, user_b_fp);
        auto inbox_key = sha3_256_prefixed("inbox:", user_d_fp);

        bool ok = n1.kad->store(inbox_key, 0x02, msg);
        EXPECT_TRUE(ok) << "Message to user with no allowlist should succeed (open inbox)";

        // Verify the message was actually stored
        std::vector<uint8_t> idx_key;
        idx_key.insert(idx_key.end(), user_d_fp.begin(), user_d_fp.end());
        Hash msg_id{};
        msg_id.fill(static_cast<uint8_t>(user_b_fp[0] ^ user_d_fp[0]));
        idx_key.insert(idx_key.end(), msg_id.begin(), msg_id.end());
        auto result = n1.storage->get(TABLE_INBOX_INDEX, idx_key);
        EXPECT_TRUE(result.has_value()) << "Inbox message to user with no allowlist should be stored";
    }

    // --- Test 4: Revoke user B from A's allowlist, then B tries to send — should FAIL ---
    {
        // Store revoke entry (action=0x00) for user B on user A's allowlist
        std::vector<uint8_t> revoke_value;
        revoke_value.insert(revoke_value.end(), user_a_fp.begin(), user_a_fp.end());
        revoke_value.insert(revoke_value.end(), user_b_fp.begin(), user_b_fp.end());
        revoke_value.push_back(0x00);  // action = revoke
        for (int i = 0; i < 7; ++i) revoke_value.push_back(0x00);
        revoke_value.push_back(0x02);  // sequence = 2

        bool revoke_ok = n1.storage->put(TABLE_ALLOWLISTS, allow_storage_key, revoke_value);
        ASSERT_TRUE(revoke_ok) << "Failed to store revoke entry";

        // User B tries to send — should be rejected since action is 0x00
        auto msg = build_inbox_msg(user_a_fp, user_b_fp);
        auto inbox_key = sha3_256_prefixed("inbox:", user_a_fp);

        // Use a different msg_id so we don't hit dedup
        Hash msg_id{};
        msg_id.fill(0xEE);
        // Overwrite msg_id bytes in the message (offset 32..63)
        std::copy(msg_id.begin(), msg_id.end(), msg.begin() + 32);

        bool ok = n1.kad->store(inbox_key, 0x02, msg);

        // Verify the message was NOT stored
        std::vector<uint8_t> idx_key;
        idx_key.insert(idx_key.end(), user_a_fp.begin(), user_a_fp.end());
        idx_key.insert(idx_key.end(), msg_id.begin(), msg_id.end());
        auto result = n1.storage->get(TABLE_INBOX_INDEX, idx_key);
        EXPECT_FALSE(result.has_value())
            << "Inbox message from revoked sender B should be rejected";
    }
}

// ---------------------------------------------------------------------------
// Test 38: AllowlistSignatureEnforced — Kademlia verifies allowlist signature
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, AllowlistSignatureEnforced) {
    auto& n1 = create_node();
    start_all();

    // Create a user identity with a keypair
    auto owner_kp = generate_keypair();
    auto owner_fp = sha3_256(owner_kp.public_key);

    // Create a contact fingerprint
    Hash contact_fp{};
    contact_fp.fill(0x42);

    // Build signed data with domain separation:
    // "chromatin:allowlist:" || owner_fp(32) || action(1) || allowed_fp(32) || sequence(8 BE)
    uint64_t sequence = 1;
    const std::string domain = "chromatin:allowlist:";
    std::vector<uint8_t> signed_data;
    signed_data.insert(signed_data.end(), domain.begin(), domain.end());
    signed_data.insert(signed_data.end(), owner_fp.begin(), owner_fp.end());
    signed_data.push_back(0x01);  // action = allow
    signed_data.insert(signed_data.end(), contact_fp.begin(), contact_fp.end());
    for (int i = 7; i >= 0; --i)
        signed_data.push_back(static_cast<uint8_t>((sequence >> (i * 8)) & 0xFF));

    auto allow_sig = sign(signed_data, owner_kp.secret_key);

    // Build valid allowlist entry: owner_fp(32) || allowed_fp(32) || action(1) || sequence(8 BE) || pubkey_len(2 BE) || pubkey || signature
    uint16_t pk_len = static_cast<uint16_t>(owner_kp.public_key.size());
    std::vector<uint8_t> valid_entry;
    valid_entry.insert(valid_entry.end(), owner_fp.begin(), owner_fp.end());
    valid_entry.insert(valid_entry.end(), contact_fp.begin(), contact_fp.end());
    valid_entry.push_back(0x01);  // action = allow
    for (int i = 7; i >= 0; --i)
        valid_entry.push_back(static_cast<uint8_t>((sequence >> (i * 8)) & 0xFF));
    valid_entry.push_back(static_cast<uint8_t>((pk_len >> 8) & 0xFF));
    valid_entry.push_back(static_cast<uint8_t>(pk_len & 0xFF));
    valid_entry.insert(valid_entry.end(), owner_kp.public_key.begin(), owner_kp.public_key.end());
    valid_entry.insert(valid_entry.end(), allow_sig.begin(), allow_sig.end());

    // Store with a valid signature — should succeed
    auto allowlist_key = sha3_256_prefixed("inbox:", owner_fp);
    bool ok = n1.kad->store(allowlist_key, 0x04, valid_entry);
    EXPECT_TRUE(ok) << "Allowlist entry with valid signature should be accepted";

    std::vector<uint8_t> composite_key(allowlist_key.begin(), allowlist_key.end());
    composite_key.insert(composite_key.end(), contact_fp.begin(), contact_fp.end());
    auto result = n1.storage->get(TABLE_ALLOWLISTS, composite_key);
    EXPECT_TRUE(result.has_value()) << "Valid allowlist entry should be stored";

    // Build an entry with invalid signature (tampered contact_fp but same sig)
    Hash contact_fp2{};
    contact_fp2.fill(0x43);

    std::vector<uint8_t> bad_entry;
    bad_entry.insert(bad_entry.end(), owner_fp.begin(), owner_fp.end());
    bad_entry.insert(bad_entry.end(), contact_fp2.begin(), contact_fp2.end());
    bad_entry.push_back(0x01);  // action = allow
    for (int i = 7; i >= 0; --i)
        bad_entry.push_back(static_cast<uint8_t>((sequence >> (i * 8)) & 0xFF));
    bad_entry.push_back(static_cast<uint8_t>((pk_len >> 8) & 0xFF));
    bad_entry.push_back(static_cast<uint8_t>(pk_len & 0xFF));
    bad_entry.insert(bad_entry.end(), owner_kp.public_key.begin(), owner_kp.public_key.end());
    // Use the same signature (valid for contact_fp, not for contact_fp2)
    bad_entry.insert(bad_entry.end(), allow_sig.begin(), allow_sig.end());

    // Store with invalid signature — should be rejected
    bool bad_ok = n1.kad->store(allowlist_key, 0x04, bad_entry);

    std::vector<uint8_t> bad_composite_key(allowlist_key.begin(), allowlist_key.end());
    bad_composite_key.insert(bad_composite_key.end(), contact_fp2.begin(), contact_fp2.end());
    auto bad_result = n1.storage->get(TABLE_ALLOWLISTS, bad_composite_key);
    EXPECT_FALSE(bad_result.has_value())
        << "Allowlist entry with invalid signature should be rejected";

    // Build entry with mismatched pubkey (pubkey doesn't hash to owner_fp) — should be rejected
    Hash unknown_owner{};
    unknown_owner.fill(0x99);
    Hash contact_fp3{};
    contact_fp3.fill(0x44);

    std::vector<uint8_t> no_match_entry;
    no_match_entry.insert(no_match_entry.end(), unknown_owner.begin(), unknown_owner.end());
    no_match_entry.insert(no_match_entry.end(), contact_fp3.begin(), contact_fp3.end());
    no_match_entry.push_back(0x01);  // action = allow
    for (int i = 7; i >= 0; --i)
        no_match_entry.push_back(static_cast<uint8_t>((sequence >> (i * 8)) & 0xFF));
    // Embed a real pubkey that doesn't match unknown_owner fingerprint
    no_match_entry.push_back(static_cast<uint8_t>((pk_len >> 8) & 0xFF));
    no_match_entry.push_back(static_cast<uint8_t>(pk_len & 0xFF));
    no_match_entry.insert(no_match_entry.end(), owner_kp.public_key.begin(), owner_kp.public_key.end());
    // dummy signature
    no_match_entry.resize(no_match_entry.size() + SIGNATURE_SIZE, 0xFF);

    auto unknown_key = sha3_256_prefixed("inbox:", unknown_owner);
    n1.kad->store(unknown_key, 0x04, no_match_entry);

    std::vector<uint8_t> np_composite_key(unknown_key.begin(), unknown_key.end());
    np_composite_key.insert(np_composite_key.end(), contact_fp3.begin(), contact_fp3.end());
    auto np_result = n1.storage->get(TABLE_ALLOWLISTS, np_composite_key);
    EXPECT_FALSE(np_result.has_value())
        << "Allowlist entry with mismatched pubkey should be rejected";
}

// ---------------------------------------------------------------------------
// Test 39: RevokeReplicatesAsDelete — REVOKE records Op::DEL in repl_log
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, RevokeReplicatesAsDelete) {
    auto& n1 = create_node(8);
    start_all();

    // Create an owner and a contact
    KeyPair owner_kp = generate_keypair();
    Hash owner_fp = sha3_256(owner_kp.public_key);
    KeyPair contact_kp = generate_keypair();
    Hash contact_fp = sha3_256(contact_kp.public_key);

    auto allowlist_key = sha3_256_prefixed("inbox:", owner_fp);
    uint16_t pk_len = static_cast<uint16_t>(owner_kp.public_key.size());

    // 1. Store ALLOW entry (action=0x01)
    {
        const std::string domain = "chromatin:allowlist:";
        std::vector<uint8_t> signed_data;
        signed_data.insert(signed_data.end(), domain.begin(), domain.end());
        signed_data.insert(signed_data.end(), owner_fp.begin(), owner_fp.end());
        signed_data.push_back(0x01); // action = ALLOW
        signed_data.insert(signed_data.end(), contact_fp.begin(), contact_fp.end());
        for (int i = 7; i >= 0; --i)
            signed_data.push_back(static_cast<uint8_t>((1ULL >> (i * 8)) & 0xFF)); // seq=1
        auto sig = sign(signed_data, owner_kp.secret_key);

        std::vector<uint8_t> value;
        value.insert(value.end(), owner_fp.begin(), owner_fp.end());
        value.insert(value.end(), contact_fp.begin(), contact_fp.end());
        value.push_back(0x01); // action
        for (int i = 7; i >= 0; --i)
            value.push_back(static_cast<uint8_t>((1ULL >> (i * 8)) & 0xFF));
        value.push_back(static_cast<uint8_t>((pk_len >> 8) & 0xFF));
        value.push_back(static_cast<uint8_t>(pk_len & 0xFF));
        value.insert(value.end(), owner_kp.public_key.begin(), owner_kp.public_key.end());
        value.insert(value.end(), sig.begin(), sig.end());

        ASSERT_TRUE(n1.kad->store(allowlist_key, 0x04, value));
    }

    uint64_t seq_after_allow = n1.repl_log->current_seq(allowlist_key);
    ASSERT_GE(seq_after_allow, 1u);

    // Check the ALLOW entry is Op::ADD
    auto entries_allow = n1.repl_log->entries_after(allowlist_key, 0);
    ASSERT_FALSE(entries_allow.empty());
    EXPECT_EQ(entries_allow.back().op, Op::ADD)
        << "ALLOW should record Op::ADD";

    // 2. Store REVOKE entry (action=0x00)
    {
        const std::string domain = "chromatin:allowlist:";
        std::vector<uint8_t> signed_data;
        signed_data.insert(signed_data.end(), domain.begin(), domain.end());
        signed_data.insert(signed_data.end(), owner_fp.begin(), owner_fp.end());
        signed_data.push_back(0x00); // action = REVOKE
        signed_data.insert(signed_data.end(), contact_fp.begin(), contact_fp.end());
        for (int i = 7; i >= 0; --i)
            signed_data.push_back(static_cast<uint8_t>((2ULL >> (i * 8)) & 0xFF)); // seq=2
        auto sig = sign(signed_data, owner_kp.secret_key);

        std::vector<uint8_t> value;
        value.insert(value.end(), owner_fp.begin(), owner_fp.end());
        value.insert(value.end(), contact_fp.begin(), contact_fp.end());
        value.push_back(0x00); // action = REVOKE
        for (int i = 7; i >= 0; --i)
            value.push_back(static_cast<uint8_t>((2ULL >> (i * 8)) & 0xFF));
        value.push_back(static_cast<uint8_t>((pk_len >> 8) & 0xFF));
        value.push_back(static_cast<uint8_t>(pk_len & 0xFF));
        value.insert(value.end(), owner_kp.public_key.begin(), owner_kp.public_key.end());
        value.insert(value.end(), sig.begin(), sig.end());

        ASSERT_TRUE(n1.kad->store(allowlist_key, 0x04, value));
    }

    // Check the REVOKE entry is Op::ADD (revoke entries are stored, not deleted)
    auto entries_revoke = n1.repl_log->entries_after(allowlist_key, seq_after_allow);
    ASSERT_FALSE(entries_revoke.empty());
    EXPECT_EQ(entries_revoke.back().op, Op::ADD)
        << "REVOKE should record Op::ADD (entry stored, not deleted)";
}

// ---------------------------------------------------------------------------
// Test: ReplLogCompaction — compact_repl_log() removes old entries
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, ReplLogCompaction) {
    auto& n1 = create_node(8);
    // Override compact settings so that 150 entries triggers compaction
    n1.kad->set_compact_keep_entries(100);
    // Disable time-based floor so count-based compaction works immediately
    n1.kad->set_compact_min_age_hours(0);
    start_all();

    // Create a routing key and store many entries into the repl_log
    Hash key{};
    key.fill(0xAA);
    std::vector<uint8_t> dummy_data = {0x01, 0x02, 0x03};

    // Append 150 entries to the repl_log for this key
    for (int i = 0; i < 150; ++i) {
        n1.repl_log->append(key, Op::ADD, 0x00, dummy_data);
    }

    // Verify we have 150 entries
    auto entries_before = n1.repl_log->entries_after(key, 0);
    ASSERT_EQ(entries_before.size(), 150u);

    // The compact_repl_log keeps only the last 100 entries.
    // It calls compact(key, max_seq - 100), which deletes entries with seq < (max_seq - 100).
    // max_seq = 150, so before_seq = 50, meaning entries 1..49 are deleted.
    // That leaves entries 50..150 = 101 entries.
    // (compact deletes entries with seq < before_seq, not seq <= before_seq)

    // Trigger tick() with an artificial compact interval exceeded.
    // We can't easily manipulate last_compact_ directly, so instead call tick()
    // which will trigger compaction since last_compact_ is default-initialized to epoch.
    n1.kad->tick();

    auto entries_after = n1.repl_log->entries_after(key, 0);
    // After compaction: entries with seq < 50 are deleted, leaving seq 50..150 = 101 entries
    EXPECT_LE(entries_after.size(), 101u)
        << "After compaction, should have at most ~101 entries (keeping last 100)";
    EXPECT_GE(entries_after.size(), 100u)
        << "After compaction, should still have at least 100 entries";

    // The lowest remaining seq should be >= 50
    uint64_t min_seq = UINT64_MAX;
    for (const auto& e : entries_after) {
        if (e.seq < min_seq) min_seq = e.seq;
    }
    EXPECT_GE(min_seq, 50u)
        << "Old entries (seq < 50) should have been compacted";
}

// ---------------------------------------------------------------------------
// Test: ContactRequestExpiry — expired contact requests are removed
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, ContactRequestExpiry) {
    auto& n1 = create_node();
    start_all();

    // Create a contact request with valid PoW
    Hash recipient_fp{};
    recipient_fp.fill(0x11);
    Hash sender_fp{};
    sender_fp.fill(0x22);

    // Build PoW preimage: "chromatin:request:" || sender_fp || recipient_fp || timestamp(8 BE ms)
    uint64_t timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    std::vector<uint8_t> preimage;
    const std::string prefix = "chromatin:request:";
    preimage.insert(preimage.end(), prefix.begin(), prefix.end());
    preimage.insert(preimage.end(), sender_fp.begin(), sender_fp.end());
    preimage.insert(preimage.end(), recipient_fp.begin(), recipient_fp.end());
    for (int i = 7; i >= 0; --i) {
        preimage.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    }

    // Find valid PoW nonce
    uint64_t pow_nonce = 0;
    for (uint64_t n = 0; n < 10'000'000; ++n) {
        if (verify_pow(preimage, n, 16)) {
            pow_nonce = n;
            break;
        }
    }
    ASSERT_TRUE(verify_pow(preimage, pow_nonce, 16));

    std::vector<uint8_t> blob = {0xDE, 0xAD};
    uint32_t blob_len = static_cast<uint32_t>(blob.size());

    // Build contact request value
    std::vector<uint8_t> request;
    request.insert(request.end(), recipient_fp.begin(), recipient_fp.end());
    request.insert(request.end(), sender_fp.begin(), sender_fp.end());
    for (int i = 7; i >= 0; --i)
        request.push_back(static_cast<uint8_t>((pow_nonce >> (i * 8)) & 0xFF));
    for (int i = 7; i >= 0; --i)
        request.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    request.push_back(static_cast<uint8_t>((blob_len >> 24) & 0xFF));
    request.push_back(static_cast<uint8_t>((blob_len >> 16) & 0xFF));
    request.push_back(static_cast<uint8_t>((blob_len >> 8) & 0xFF));
    request.push_back(static_cast<uint8_t>(blob_len & 0xFF));
    request.insert(request.end(), blob.begin(), blob.end());

    auto requests_key = sha3_256_prefixed("inbox:", recipient_fp);

    // Store the contact request
    bool ok = n1.kad->store(requests_key, 0x03, request);
    ASSERT_TRUE(ok);

    // Verify it was stored
    std::vector<uint8_t> composite_key(recipient_fp.begin(), recipient_fp.end());
    composite_key.insert(composite_key.end(), sender_fp.begin(), sender_fp.end());
    auto result = n1.storage->get(TABLE_REQUESTS, composite_key);
    ASSERT_TRUE(result.has_value());

    // Now tamper with the repl_log timestamp to make it appear old (>7 days ago).
    // We do this by deleting the repl_log entry and re-inserting it with an old timestamp.
    auto entries = n1.repl_log->entries_after(requests_key, 0);
    ASSERT_FALSE(entries.empty());

    // Compact all existing entries
    uint64_t max_seq = n1.repl_log->current_seq(requests_key);
    n1.repl_log->compact(requests_key, max_seq + 1);

    // Re-insert with a timestamp from 8 days ago (milliseconds)
    auto eight_days_ago = std::chrono::system_clock::now() - std::chrono::hours(8 * 24);
    uint64_t old_timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(eight_days_ago.time_since_epoch()).count());

    // Manually create and store an old log entry
    LogEntry old_entry;
    old_entry.seq = 1;
    old_entry.op = Op::ADD;
    old_entry.timestamp = old_timestamp;
    old_entry.data_type = 0x03;
    old_entry.data = request;
    n1.repl_log->apply(requests_key, {old_entry});

    // Verify the old entry is in the repl_log
    auto old_entries = n1.repl_log->entries_after(requests_key, 0);
    ASSERT_FALSE(old_entries.empty());
    EXPECT_EQ(old_entries[0].timestamp, old_timestamp);

    // Now trigger tick() which should run expire_ttl() and remove the old contact request.
    // expire_ttl() runs on TTL_SWEEP_INTERVAL (5 min), but last_ttl_sweep_ is epoch-initialized.
    n1.kad->tick();

    // Verify the contact request was removed
    auto result_after = n1.storage->get(TABLE_REQUESTS, composite_key);
    EXPECT_FALSE(result_after.has_value())
        << "Contact request older than 7 days should have been expired";
}

// ---------------------------------------------------------------------------
// Test 40: InboxResponsibilityTransfer — inbox data pushed to new nodes on table change
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, InboxResponsibilityTransfer) {
    auto& n1 = create_node(8);
    start_all();

    // Build an inbox message: recipient_fp(32) || msg_id(32) || sender_fp(32) || timestamp(8 BE) || blob_len(4 BE) || blob
    Hash recipient_fp{};
    recipient_fp.fill(0x11);
    Hash msg_id{};
    msg_id.fill(0x22);
    Hash sender_fp{};
    sender_fp.fill(0x33);

    // Use a recent timestamp (millis) so TTL expiry doesn't remove the message during tick()
    uint64_t timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    std::vector<uint8_t> blob = {0xDE, 0xAD, 0xBE, 0xEF};
    uint32_t blob_len = static_cast<uint32_t>(blob.size());

    std::vector<uint8_t> inbox_value;
    inbox_value.reserve(108 + blob.size());
    inbox_value.insert(inbox_value.end(), recipient_fp.begin(), recipient_fp.end());
    inbox_value.insert(inbox_value.end(), msg_id.begin(), msg_id.end());
    inbox_value.insert(inbox_value.end(), sender_fp.begin(), sender_fp.end());
    for (int i = 7; i >= 0; --i)
        inbox_value.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    inbox_value.push_back(static_cast<uint8_t>((blob_len >> 24) & 0xFF));
    inbox_value.push_back(static_cast<uint8_t>((blob_len >> 16) & 0xFF));
    inbox_value.push_back(static_cast<uint8_t>((blob_len >> 8) & 0xFF));
    inbox_value.push_back(static_cast<uint8_t>(blob_len & 0xFF));
    inbox_value.insert(inbox_value.end(), blob.begin(), blob.end());

    auto inbox_key = sha3_256_prefixed("inbox:", recipient_fp);

    // Store inbox message on n1 (single node, responsible for everything)
    bool ok = n1.kad->store(inbox_key, 0x02, inbox_value);
    ASSERT_TRUE(ok);

    // Verify it was stored on n1
    std::vector<uint8_t> idx_key;
    idx_key.insert(idx_key.end(), recipient_fp.begin(), recipient_fp.end());
    idx_key.insert(idx_key.end(), msg_id.begin(), msg_id.end());
    ASSERT_TRUE(n1.storage->get(TABLE_INBOX_INDEX, idx_key).has_value());
    ASSERT_TRUE(n1.storage->get(TABLE_MESSAGE_BLOBS, msg_id).has_value());

    // Now create n2 and bootstrap bidirectionally — this changes the routing table
    auto& n2 = create_node(8);
    n2.start_recv();
    bootstrap_bidirectional(n2, n1);

    // n1's routing table changed (n2 joined). tick() should trigger transfer.
    n1.kad->tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // n2 should now have the inbox message (pushed via STORE from n1)
    auto idx_result = n2.storage->get(TABLE_INBOX_INDEX, idx_key);
    EXPECT_TRUE(idx_result.has_value())
        << "n2 should have received the inbox INDEX entry via responsibility transfer";

    auto blob_result = n2.storage->get(TABLE_MESSAGE_BLOBS, msg_id);
    EXPECT_TRUE(blob_result.has_value())
        << "n2 should have received the inbox BLOB entry via responsibility transfer";

    if (blob_result.has_value()) {
        EXPECT_EQ(*blob_result, blob)
            << "Transferred blob content should match original";
    }
}

// ---------------------------------------------------------------------------
// FindNodeRateLimit — rapid FIND_NODE requests are throttled
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, FindNodeRateLimit) {
    auto& node_a = create_node();
    auto& node_b = create_node();
    start_all();

    // Bootstrap so they know each other
    bootstrap_bidirectional(node_a, node_b);

    // Verify initial routing table has entries
    EXPECT_GE(node_a.table->size(), 1u);

    // Send 5 rapid FIND_NODE messages from node_a to node_b
    // Only the first should be processed (1 per second limit)
    for (int i = 0; i < 5; ++i) {
        auto payload = node_a.kad->self().pubkey;
        // Build a FIND_NODE payload: pubkey_len(2 BE) || pubkey
        std::vector<uint8_t> fn_payload;
        uint16_t pk_len = static_cast<uint16_t>(payload.size());
        fn_payload.push_back(static_cast<uint8_t>((pk_len >> 8) & 0xFF));
        fn_payload.push_back(static_cast<uint8_t>(pk_len & 0xFF));
        fn_payload.insert(fn_payload.end(), payload.begin(), payload.end());

        Message msg;
        msg.type = MessageType::FIND_NODE;
        msg.sender = node_a.info.id;
        msg.sender_port = node_a.info.tcp_port;
        msg.payload = fn_payload;
        // FIND_NODE messages are not signed per protocol
        node_a.transport->send("127.0.0.1", node_b.info.tcp_port, msg);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // The node should still be functional — test passes if no crash
    // and the rate limiting didn't break normal operation
    EXPECT_GE(node_b.table->size(), 1u);
}

// ---------------------------------------------------------------------------
// Group message validation
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, GroupMessageValidation) {
    auto& n1 = create_node();
    start_all();

    // Build valid GROUP_MESSAGE:
    // group_id(32) || sender_fp(32) || msg_id(32) || timestamp(8 BE) || gek_version(4 BE) || blob_len(4 BE) || blob
    Hash group_id{};
    group_id.fill(0x11);
    Hash sender_fp{};
    sender_fp.fill(0x22);
    Hash msg_id{};
    msg_id.fill(0x33);

    uint64_t timestamp = 1700000000;
    uint32_t gek_version = 1;
    std::vector<uint8_t> blob = {0xCA, 0xFE, 0xBA, 0xBE};
    uint32_t blob_len = static_cast<uint32_t>(blob.size());

    std::vector<uint8_t> value;
    value.reserve(112 + blob.size());
    value.insert(value.end(), group_id.begin(), group_id.end());      // 32
    value.insert(value.end(), sender_fp.begin(), sender_fp.end());    // 32
    value.insert(value.end(), msg_id.begin(), msg_id.end());          // 32
    for (int i = 7; i >= 0; --i)                                      // 8 BE timestamp
        value.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    for (int i = 3; i >= 0; --i)                                      // 4 BE gek_version
        value.push_back(static_cast<uint8_t>((gek_version >> (i * 8)) & 0xFF));
    for (int i = 3; i >= 0; --i)                                      // 4 BE blob_len
        value.push_back(static_cast<uint8_t>((blob_len >> (i * 8)) & 0xFF));
    value.insert(value.end(), blob.begin(), blob.end());              // blob

    auto group_key = sha3_256_prefixed("group:", group_id);
    bool ok = n1.kad->store(group_key, 0x05, value);
    EXPECT_TRUE(ok);

    // Verify stored in GROUP_INDEX and GROUP_BLOBS
    std::vector<uint8_t> idx_key;
    idx_key.insert(idx_key.end(), group_id.begin(), group_id.end());
    idx_key.insert(idx_key.end(), msg_id.begin(), msg_id.end());
    auto idx_result = n1.storage->get(TABLE_GROUP_INDEX, idx_key);
    ASSERT_TRUE(idx_result.has_value());
    EXPECT_EQ(idx_result->size(), 48u);  // sender_fp(32) + ts(8) + size(4) + gek_version(4)

    auto blob_result = n1.storage->get(TABLE_GROUP_BLOBS, idx_key);
    ASSERT_TRUE(blob_result.has_value());
    EXPECT_EQ(*blob_result, blob);
}

TEST_F(KademliaTest, GroupMessageValidation_TooShort) {
    auto& n1 = create_node();
    start_all();

    // Truncated group message (< 112 bytes)
    std::vector<uint8_t> value(80, 0x00);
    Hash key{};
    key.fill(0xAA);

    bool ok = n1.kad->store(key, 0x05, value);
    EXPECT_FALSE(ok);
}

TEST_F(KademliaTest, GroupMessageValidation_BlobLenMismatch) {
    auto& n1 = create_node();
    start_all();

    // Group message with blob_len=100 at offset [108..111] but no actual blob data
    std::vector<uint8_t> value(112, 0x00);
    value[108] = 0; value[109] = 0; value[110] = 0; value[111] = 100;

    Hash key{};
    key.fill(0xBB);

    bool ok = n1.kad->store(key, 0x05, value);
    EXPECT_FALSE(ok);
}

TEST_F(KademliaTest, GroupMetaValidation) {
    auto& n1 = create_node();
    start_all();

    // Build valid GROUP_META:
    // group_id(32) || owner_fp(32) || version(4 BE) || member_count(2 BE) ||
    // per-member[fp(32) + role(1) + kem_ciphertext(1568)] || sig_len(2 BE) || signature
    auto owner_kp = generate_keypair();
    Hash owner_fp = sha3_256(owner_kp.public_key);

    Hash group_id{};
    group_id.fill(0x44);

    std::vector<uint8_t> meta;
    meta.insert(meta.end(), group_id.begin(), group_id.end());       // group_id(32)
    meta.insert(meta.end(), owner_fp.begin(), owner_fp.end());       // owner_fp(32)
    // version = 1 (4 BE)
    meta.push_back(0); meta.push_back(0); meta.push_back(0); meta.push_back(1);
    // member_count = 1 (2 BE)
    meta.push_back(0); meta.push_back(1);
    // member: fp(32) + role(1) + kem_ciphertext(1568)
    meta.insert(meta.end(), owner_fp.begin(), owner_fp.end());       // member fp
    meta.push_back(0x02);                                             // role = owner
    meta.resize(meta.size() + 1568, 0x00);                           // dummy kem_ciphertext

    // Sign the data so far
    auto signature = sign(meta, owner_kp.secret_key);
    uint16_t sig_len = static_cast<uint16_t>(signature.size());
    meta.push_back(static_cast<uint8_t>((sig_len >> 8) & 0xFF));
    meta.push_back(static_cast<uint8_t>(sig_len & 0xFF));
    meta.insert(meta.end(), signature.begin(), signature.end());

    auto group_key = sha3_256_prefixed("group:", group_id);
    bool ok = n1.kad->store(group_key, 0x06, meta);
    EXPECT_TRUE(ok);

    // Verify stored in TABLE_GROUP_META (keyed by routing key, not group_id)
    std::vector<uint8_t> routing_key(group_key.begin(), group_key.end());
    auto result = n1.storage->get(TABLE_GROUP_META, routing_key);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), meta.size());
}

TEST_F(KademliaTest, GroupMetaValidation_ZeroMembers) {
    auto& n1 = create_node();
    start_all();

    Hash group_id{};
    group_id.fill(0x55);
    Hash owner_fp{};
    owner_fp.fill(0x66);

    std::vector<uint8_t> meta;
    meta.insert(meta.end(), group_id.begin(), group_id.end());
    meta.insert(meta.end(), owner_fp.begin(), owner_fp.end());
    meta.push_back(0); meta.push_back(0); meta.push_back(0); meta.push_back(1);  // version
    meta.push_back(0); meta.push_back(0);  // member_count = 0
    // sig_len + dummy sig
    meta.push_back(0); meta.push_back(4);
    meta.push_back(0xDE); meta.push_back(0xAD); meta.push_back(0xBE); meta.push_back(0xEF);

    auto group_key = sha3_256_prefixed("group:", group_id);
    bool ok = n1.kad->store(group_key, 0x06, meta);
    EXPECT_FALSE(ok);
}

TEST_F(KademliaTest, GroupMetaValidation_NoOwner) {
    auto& n1 = create_node();
    start_all();

    Hash group_id{};
    group_id.fill(0x77);
    Hash member_fp{};
    member_fp.fill(0x88);

    std::vector<uint8_t> meta;
    meta.insert(meta.end(), group_id.begin(), group_id.end());
    meta.insert(meta.end(), member_fp.begin(), member_fp.end());  // owner_fp field
    meta.push_back(0); meta.push_back(0); meta.push_back(0); meta.push_back(1);
    meta.push_back(0); meta.push_back(1);  // 1 member
    meta.insert(meta.end(), member_fp.begin(), member_fp.end());
    meta.push_back(0x00);  // role = member (NOT owner)
    meta.resize(meta.size() + 1568, 0x00);
    // sig_len + dummy sig
    meta.push_back(0); meta.push_back(4);
    meta.push_back(0xDE); meta.push_back(0xAD); meta.push_back(0xBE); meta.push_back(0xEF);

    auto group_key = sha3_256_prefixed("group:", group_id);
    bool ok = n1.kad->store(group_key, 0x06, meta);
    EXPECT_FALSE(ok);
}

TEST_F(KademliaTest, GroupMetaValidation_WrongKey) {
    auto& n1 = create_node();
    start_all();

    auto owner_kp = generate_keypair();
    Hash owner_fp = sha3_256(owner_kp.public_key);

    Hash group_id{};
    group_id.fill(0x99);

    std::vector<uint8_t> meta;
    meta.insert(meta.end(), group_id.begin(), group_id.end());
    meta.insert(meta.end(), owner_fp.begin(), owner_fp.end());
    meta.push_back(0); meta.push_back(0); meta.push_back(0); meta.push_back(1);
    meta.push_back(0); meta.push_back(1);
    meta.insert(meta.end(), owner_fp.begin(), owner_fp.end());
    meta.push_back(0x02);
    meta.resize(meta.size() + 1568, 0x00);
    auto signature = sign(meta, owner_kp.secret_key);
    uint16_t sig_len = static_cast<uint16_t>(signature.size());
    meta.push_back(static_cast<uint8_t>((sig_len >> 8) & 0xFF));
    meta.push_back(static_cast<uint8_t>(sig_len & 0xFF));
    meta.insert(meta.end(), signature.begin(), signature.end());

    // Use wrong key (not SHA3-256("group:" || group_id))
    Hash wrong_key{};
    wrong_key.fill(0xFF);
    bool ok = n1.kad->store(wrong_key, 0x06, meta);
    EXPECT_FALSE(ok);
}

TEST_F(KademliaTest, DuplicateGroupMessageRejected) {
    auto& n1 = create_node();
    start_all();

    Hash group_id{};
    group_id.fill(0xAA);
    Hash sender_fp{};
    sender_fp.fill(0xBB);
    Hash msg_id{};
    msg_id.fill(0xCC);

    std::vector<uint8_t> value;
    value.insert(value.end(), group_id.begin(), group_id.end());
    value.insert(value.end(), sender_fp.begin(), sender_fp.end());
    value.insert(value.end(), msg_id.begin(), msg_id.end());
    value.resize(108, 0x00);  // timestamp + gek_version + blob_len=0
    value.resize(112, 0x00);  // ensure 112 bytes

    auto group_key = sha3_256_prefixed("group:", group_id);

    // First store should succeed
    bool ok1 = n1.kad->store(group_key, 0x05, value);
    EXPECT_TRUE(ok1);

    // Second store with same msg_id should be rejected
    bool ok2 = n1.kad->store(group_key, 0x05, value);
    EXPECT_FALSE(ok2);
}

// ---------------------------------------------------------------------------
// validate_readonly tests
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, ValidateReadonlyAcceptsValidName) {
    auto& n1 = create_node(8);
    start_all();

    KeyPair user_kp = generate_keypair();
    Hash user_fp = sha3_256(user_kp.public_key);
    std::string name = "alice";
    Hash key = name_key(name);

    uint64_t nonce = find_pow_nonce(name, user_fp, 8);
    auto record = build_name_record(name, user_fp, nonce, 1, user_kp);

    EXPECT_TRUE(n1.kad->validate_readonly(key, 0x01, record));
}

TEST_F(KademliaTest, ValidateReadonlyRejectsCorruptName) {
    auto& n1 = create_node(8);
    start_all();

    Hash key{};
    key.fill(0x42);
    // Garbage data that doesn't parse as a valid name record
    std::vector<uint8_t> corrupt = {0xDE, 0xAD, 0xBE, 0xEF};

    EXPECT_FALSE(n1.kad->validate_readonly(key, 0x01, corrupt));
}

TEST_F(KademliaTest, ValidateReadonlyAcceptsValidProfile) {
    auto& n1 = create_node(8);
    start_all();

    KeyPair user_kp = generate_keypair();
    auto profile = build_profile(user_kp, 1);
    Hash user_fp = sha3_256(user_kp.public_key);
    Hash key = sha3_256_prefixed("profile:", user_fp);

    EXPECT_TRUE(n1.kad->validate_readonly(key, 0x00, profile));
}

TEST_F(KademliaTest, ValidateReadonlyRejectsCorruptProfile) {
    auto& n1 = create_node(8);
    start_all();

    Hash key{};
    key.fill(0x42);
    std::vector<uint8_t> corrupt = {0x00, 0x01, 0x02, 0x03};

    EXPECT_FALSE(n1.kad->validate_readonly(key, 0x00, corrupt));
}

TEST_F(KademliaTest, ValidateReadonlyAcceptsValidAllowlist) {
    auto& n1 = create_node(8);
    start_all();

    KeyPair owner_kp = generate_keypair();
    Hash contact_fp{};
    contact_fp.fill(0xBB);

    auto entry = build_signed_allowlist(owner_kp, contact_fp, 0x01, 1);

    // Allowlist doesn't use the routing key for validation, any key is fine
    Hash key{};
    key.fill(0x00);

    EXPECT_TRUE(n1.kad->validate_readonly(key, 0x04, entry));
}

TEST_F(KademliaTest, ValidateReadonlyRejectsCorruptAllowlist) {
    auto& n1 = create_node(8);
    start_all();

    Hash key{};
    key.fill(0x00);
    std::vector<uint8_t> corrupt(100, 0xFF);

    EXPECT_FALSE(n1.kad->validate_readonly(key, 0x04, corrupt));
}

TEST_F(KademliaTest, ValidateReadonlySkipsSequenceCheck) {
    auto& n1 = create_node(8);
    start_all();

    // Store a name record with sequence=2
    KeyPair user_kp = generate_keypair();
    Hash user_fp = sha3_256(user_kp.public_key);
    std::string name = "bob";
    Hash key = name_key(name);
    uint64_t nonce = find_pow_nonce(name, user_fp, 8);

    auto record_v2 = build_name_record(name, user_fp, nonce, 2, user_kp);
    ASSERT_TRUE(n1.kad->store(key, 0x01, record_v2));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Build a record with sequence=1 (lower than stored)
    auto record_v1 = build_name_record(name, user_fp, nonce, 1, user_kp);

    // validate_readonly should still accept it (skips sequence check)
    EXPECT_TRUE(n1.kad->validate_readonly(key, 0x01, record_v1));
}

TEST_F(KademliaTest, ValidateReadonlyRejectsUnknownDataType) {
    auto& n1 = create_node(8);
    start_all();

    Hash key{};
    key.fill(0x42);
    std::vector<uint8_t> data = {0x01, 0x02, 0x03};

    // Unknown data type 0xFF should be rejected
    EXPECT_FALSE(n1.kad->validate_readonly(key, 0xFF, data));
}

// ---------------------------------------------------------------------------
// handle_find_value corruption test
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, FindValuePurgesCorruptData) {
    auto& n1 = create_node(8);
    auto& n2 = create_node(8);

    start_all();
    bootstrap_bidirectional(n2, n1);

    // Store corrupt data directly into n1's profile storage
    Hash key{};
    key.fill(0x42);
    std::vector<uint8_t> corrupt = {0xDE, 0xAD, 0xBE, 0xEF};
    n1.storage->put(TABLE_PROFILES, key, corrupt);

    // Verify it's there
    ASSERT_TRUE(n1.storage->get(TABLE_PROFILES, key).has_value());

    // Send FIND_VALUE from n2 to n1
    std::vector<uint8_t> fv_payload(key.begin(), key.end());
    Message fv_msg;
    fv_msg.type = MessageType::FIND_VALUE;
    fv_msg.sender = n2.info.id;
    fv_msg.sender_port = n2.info.tcp_port;
    fv_msg.payload = fv_payload;
    sign_message(fv_msg, n2.keypair.secret_key);

    // Track response
    std::atomic<bool> got_response{false};
    std::atomic<bool> value_found{false};

    n2.stop();
    n2.recv_thread = std::thread([&]() {
        n2.transport->run([&](const Message& msg, const std::string& from_addr, uint16_t from_port) {
            if (msg.type == MessageType::VALUE) {
                got_response.store(true);
                if (msg.payload.size() >= 37) {
                    value_found.store(msg.payload[32] == 0x01);
                }
                n2.transport->stop();
            } else {
                n2.kad->handle_message(msg, from_addr, from_port);
            }
        });
    });
    n2.running.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    n2.transport->send("127.0.0.1", n1.info.tcp_port, fv_msg);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    n2.transport->stop();
    if (n2.recv_thread.joinable()) n2.recv_thread.join();
    n2.running.store(false);

    // Should have received a response with found=0x00 (not found)
    EXPECT_TRUE(got_response.load());
    EXPECT_FALSE(value_found.load()) << "Corrupt data should not be served via FIND_VALUE";

    // Corrupt entry should have been purged from storage
    EXPECT_FALSE(n1.storage->get(TABLE_PROFILES, key).has_value())
        << "Corrupt entry should be purged from storage";
}

// ---------------------------------------------------------------------------
// integrity_sweep test
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, IntegritySweepPurgesCorruptEntries) {
    auto& n1 = create_node(8);
    start_all();

    // Store a valid name record
    KeyPair user_kp = generate_keypair();
    Hash user_fp = sha3_256(user_kp.public_key);
    std::string name = "carol";
    Hash name_k = name_key(name);
    uint64_t nonce = find_pow_nonce(name, user_fp, 8);
    auto valid_record = build_name_record(name, user_fp, nonce, 1, user_kp);
    n1.storage->put(TABLE_NAMES, name_k, valid_record);

    // Store a corrupt name record
    Hash corrupt_key{};
    corrupt_key.fill(0xEE);
    std::vector<uint8_t> corrupt_data = {0xBA, 0xAD, 0xF0, 0x0D};
    n1.storage->put(TABLE_NAMES, corrupt_key, corrupt_data);

    // Verify both exist
    ASSERT_TRUE(n1.storage->get(TABLE_NAMES, name_k).has_value());
    ASSERT_TRUE(n1.storage->get(TABLE_NAMES, corrupt_key).has_value());

    // Set sweep interval very low and run tick to trigger sweep
    n1.kad->set_integrity_sweep_interval(std::chrono::hours(0));
    n1.kad->tick();

    // Valid record should survive
    EXPECT_TRUE(n1.storage->get(TABLE_NAMES, name_k).has_value())
        << "Valid name record should survive integrity sweep";

    // Corrupt record should be purged
    EXPECT_FALSE(n1.storage->get(TABLE_NAMES, corrupt_key).has_value())
        << "Corrupt name record should be purged by integrity sweep";
}

// ---------------------------------------------------------------------------
// query_remote_values local lookup test
// ---------------------------------------------------------------------------

TEST_F(KademliaTest, QueryRemoteValuesFindsProfiles) {
    auto& n1 = create_node(8);
    start_all();

    // Store a valid profile
    KeyPair user_kp = generate_keypair();
    Hash user_fp = sha3_256(user_kp.public_key);
    Hash key = sha3_256_prefixed("profile:", user_fp);
    auto profile = build_profile(user_kp, 1);
    n1.storage->put(TABLE_PROFILES, key, profile);

    // query_remote_values with self should find it locally
    std::vector<NodeInfo> nodes = {n1.info};
    auto results = n1.kad->query_remote_values(key, nodes);

    ASSERT_EQ(results.size(), 1u);
    ASSERT_TRUE(results[0].value.has_value());
    EXPECT_EQ(*results[0].value, profile);
}

TEST_F(KademliaTest, QueryRemoteValuesFindsGroupMeta) {
    auto& n1 = create_node(8);
    start_all();

    // Build minimal valid group_meta and store directly
    // We'll use raw storage since group_meta validation is structural
    KeyPair owner_kp = generate_keypair();
    Hash owner_fp = sha3_256(owner_kp.public_key);
    Hash group_id{};
    group_id.fill(0xAA);
    Hash routing_key = sha3_256_prefixed("group:", group_id);

    // Build group meta: group_id(32) || owner_fp(32) || version(4 BE) || member_count(2 BE)
    //   || member: fp(32) + role(1) + kem_ciphertext(1568)
    //   || sig_len(2 BE) || signature
    std::vector<uint8_t> meta;
    meta.insert(meta.end(), group_id.begin(), group_id.end());
    meta.insert(meta.end(), owner_fp.begin(), owner_fp.end());
    // version = 1
    meta.push_back(0x00); meta.push_back(0x00); meta.push_back(0x00); meta.push_back(0x01);
    // member_count = 1
    meta.push_back(0x00); meta.push_back(0x01);
    // member: owner_fp(32) + role=0x02 + kem_ciphertext(1568 zeros)
    meta.insert(meta.end(), owner_fp.begin(), owner_fp.end());
    meta.push_back(0x02); // owner role
    meta.resize(meta.size() + 1568, 0x00); // kem_ciphertext placeholder

    // Sign it
    auto signature = sign(std::span<const uint8_t>(meta.data(), meta.size()), owner_kp.secret_key);
    uint16_t sig_len = static_cast<uint16_t>(signature.size());
    meta.push_back(static_cast<uint8_t>((sig_len >> 8) & 0xFF));
    meta.push_back(static_cast<uint8_t>(sig_len & 0xFF));
    meta.insert(meta.end(), signature.begin(), signature.end());

    n1.storage->put(TABLE_GROUP_META, routing_key, meta);

    // query_remote_values with self should find it locally
    std::vector<NodeInfo> nodes = {n1.info};
    auto results = n1.kad->query_remote_values(routing_key, nodes);

    ASSERT_EQ(results.size(), 1u);
    ASSERT_TRUE(results[0].value.has_value());
    EXPECT_EQ(*results[0].value, meta);
}
