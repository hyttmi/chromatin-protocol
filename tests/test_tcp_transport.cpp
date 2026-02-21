#include <gtest/gtest.h>

#include "crypto/crypto.h"
#include "kademlia/node_id.h"
#include "kademlia/tcp_transport.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

using namespace chromatin::kademlia;
using namespace chromatin::crypto;

// ---------------------------------------------------------------------------
// Helper: create a test message with a known sender and payload
// ---------------------------------------------------------------------------

static Message make_test_message(MessageType type, const NodeId& sender,
                                 const std::vector<uint8_t>& payload = {},
                                 uint16_t sender_port = 0) {
    Message msg;
    msg.type = type;
    msg.sender = sender;
    msg.sender_port = sender_port;
    msg.payload = payload;
    return msg;
}

// ---------------------------------------------------------------------------
// Serialization round-trip
// ---------------------------------------------------------------------------

TEST(TcpTransport, SerializeDeserializeRoundTrip) {
    NodeId sender;
    sender.id.fill(0xAB);

    std::vector<uint8_t> payload = {0x01, 0x02, 0x03, 0x04, 0x05};
    std::vector<uint8_t> fake_sig = {0xDE, 0xAD, 0xBE, 0xEF};

    Message msg;
    msg.type = MessageType::STORE;
    msg.sender = sender;
    msg.sender_port = 4000;
    msg.payload = payload;
    msg.signature = fake_sig;

    auto serialized = serialize_message(msg);
    auto result = deserialize_message(serialized);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type, MessageType::STORE);
    EXPECT_EQ(result->sender, sender);
    EXPECT_EQ(result->sender_port, 4000);
    EXPECT_EQ(result->payload, payload);
    EXPECT_EQ(result->signature, fake_sig);
}

// ---------------------------------------------------------------------------
// Invalid magic
// ---------------------------------------------------------------------------

TEST(TcpTransport, DeserializeInvalidMagic) {
    NodeId sender;
    sender.id.fill(0x01);

    Message msg = make_test_message(MessageType::PING, sender);
    auto serialized = serialize_message(msg);

    // Corrupt magic bytes
    serialized[0] = 'X';
    serialized[1] = 'X';

    auto result = deserialize_message(serialized);
    EXPECT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// Invalid version
// ---------------------------------------------------------------------------

TEST(TcpTransport, DeserializeInvalidVersion) {
    NodeId sender;
    sender.id.fill(0x02);

    Message msg = make_test_message(MessageType::PING, sender);
    auto serialized = serialize_message(msg);

    // Version byte is at offset 4 (after 4-byte CHRM magic)
    serialized[4] = 0xFF;

    auto result = deserialize_message(serialized);
    EXPECT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// Truncated data
// ---------------------------------------------------------------------------

TEST(TcpTransport, DeserializeTruncatedData) {
    // Too short to even contain a header
    std::vector<uint8_t> short_data = {'C', 'H', 'R', 'M', 0x01, 0x00};
    auto result = deserialize_message(short_data);
    EXPECT_FALSE(result.has_value());

    // Has header but payload length claims more data than available
    NodeId sender;
    sender.id.fill(0x03);
    Message msg = make_test_message(MessageType::PING, sender, {0x01, 0x02, 0x03});
    auto serialized = serialize_message(msg);

    // Truncate to just after the payload length field (remove payload + sig)
    std::vector<uint8_t> truncated(serialized.begin(), serialized.begin() + HEADER_SIZE);
    result = deserialize_message(truncated);
    EXPECT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// Sign and verify with real ML-DSA-87 keys
// ---------------------------------------------------------------------------

TEST(TcpTransport, SignAndVerifyMessage) {
    KeyPair kp = generate_keypair();
    NodeId sender = NodeId::from_pubkey(kp.public_key);

    Message msg = make_test_message(MessageType::FIND_NODE, sender, {0xCA, 0xFE}, 5000);
    sign_message(msg, kp.secret_key);

    EXPECT_FALSE(msg.signature.empty());
    EXPECT_TRUE(verify_message(msg, kp.public_key));
}

// ---------------------------------------------------------------------------
// Verify rejects tampered payload
// ---------------------------------------------------------------------------

TEST(TcpTransport, VerifyRejectsTamperedPayload) {
    KeyPair kp = generate_keypair();
    NodeId sender = NodeId::from_pubkey(kp.public_key);

    Message msg = make_test_message(MessageType::STORE, sender, {0x01, 0x02, 0x03});
    sign_message(msg, kp.secret_key);

    EXPECT_TRUE(verify_message(msg, kp.public_key));

    // Tamper with the payload
    msg.payload[0] = 0xFF;

    EXPECT_FALSE(verify_message(msg, kp.public_key));
}

// ---------------------------------------------------------------------------
// Verify rejects wrong key
// ---------------------------------------------------------------------------

TEST(TcpTransport, VerifyRejectsWrongKey) {
    KeyPair kp1 = generate_keypair();
    KeyPair kp2 = generate_keypair();
    NodeId sender = NodeId::from_pubkey(kp1.public_key);

    Message msg = make_test_message(MessageType::PING, sender);
    sign_message(msg, kp1.secret_key);

    // Verify with wrong public key
    EXPECT_FALSE(verify_message(msg, kp2.public_key));
}

// ---------------------------------------------------------------------------
// Loopback send/recv test over TCP
// ---------------------------------------------------------------------------

TEST(TcpTransport, SendRecvLoopback) {
    TcpTransport transport("127.0.0.1", 0);
    uint16_t port = transport.local_port();
    ASSERT_GT(port, 0);

    NodeId sender;
    sender.id.fill(0x42);
    std::vector<uint8_t> payload = {0x11, 0x22, 0x33};

    Message sent_msg;
    sent_msg.type = MessageType::FIND_VALUE;
    sent_msg.sender = sender;
    sent_msg.sender_port = port;
    sent_msg.payload = payload;
    sent_msg.signature = {0xAA, 0xBB};

    Message received_msg;
    std::string received_addr;
    uint16_t received_port = 0;
    bool got_message = false;

    // Start receiver in a thread
    std::thread recv_thread([&]() {
        transport.run([&](const Message& msg, const std::string& from_addr, uint16_t from_port) {
            received_msg = msg;
            received_addr = from_addr;
            received_port = from_port;
            got_message = true;
            transport.stop();
        });
    });

    // Give the accept loop time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send to self
    transport.send("127.0.0.1", port, sent_msg);

    // Wait for recv thread to finish (with timeout)
    if (recv_thread.joinable()) {
        recv_thread.join();
    }

    ASSERT_TRUE(got_message) << "Did not receive message via loopback";
    EXPECT_EQ(received_msg.type, MessageType::FIND_VALUE);
    EXPECT_EQ(received_msg.sender, sender);
    EXPECT_EQ(received_msg.sender_port, port);
    EXPECT_EQ(received_msg.payload, payload);
    EXPECT_EQ(received_msg.signature, (std::vector<uint8_t>{0xAA, 0xBB}));
    EXPECT_EQ(received_addr, "127.0.0.1");
    EXPECT_EQ(received_port, port);
}

// ---------------------------------------------------------------------------
// Connection pooling: multiple messages reuse the same connection
// ---------------------------------------------------------------------------

TEST(TcpTransport, ConnectionPooling) {
    // Use two separate transports: sender and receiver
    TcpTransport receiver("127.0.0.1", 0);
    TcpTransport sender("127.0.0.1", 0);
    uint16_t recv_port = receiver.local_port();
    ASSERT_GT(recv_port, 0);

    constexpr int NUM_MESSAGES = 5;
    std::atomic<int> recv_count{0};
    std::vector<Message> received_msgs;
    std::mutex recv_mutex;

    // Start receiver in a thread — it will read multiple messages per connection
    std::thread recv_thread([&]() {
        receiver.run([&](const Message& msg, const std::string& /*from_addr*/,
                         uint16_t /*from_port*/) {
            {
                std::lock_guard lock(recv_mutex);
                received_msgs.push_back(msg);
            }
            if (recv_count.fetch_add(1) + 1 >= NUM_MESSAGES) {
                receiver.stop();
            }
        });
    });

    // Give the accept loop time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send multiple messages from sender to receiver
    NodeId node_id;
    node_id.id.fill(0x55);

    for (int i = 0; i < NUM_MESSAGES; ++i) {
        Message msg;
        msg.type = MessageType::PING;
        msg.sender = node_id;
        msg.sender_port = sender.local_port();
        msg.payload = {static_cast<uint8_t>(i)};
        msg.signature = {0xCC};
        sender.send("127.0.0.1", recv_port, msg);
        // Small delay to ensure ordering
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Wait for recv thread (with timeout safety)
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (recv_count.load() < NUM_MESSAGES &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    receiver.stop();
    if (recv_thread.joinable()) {
        recv_thread.join();
    }

    ASSERT_EQ(recv_count.load(), NUM_MESSAGES)
        << "Expected " << NUM_MESSAGES << " messages but received " << recv_count.load();

    // Verify each message payload
    std::lock_guard lock(recv_mutex);
    for (int i = 0; i < NUM_MESSAGES; ++i) {
        EXPECT_EQ(received_msgs[i].payload, (std::vector<uint8_t>{static_cast<uint8_t>(i)}))
            << "Payload mismatch for message " << i;
    }
}

// ---------------------------------------------------------------------------
// Cleanup idle connections closes stale pooled fds
// ---------------------------------------------------------------------------

TEST(TcpTransport, CleanupIdleConnections) {
    // Verify that cleanup_idle_connections() can be called without crashing,
    // and that the public API is accessible.
    TcpTransport transport("127.0.0.1", 0);

    // Should be a no-op when pool is empty
    transport.cleanup_idle_connections();

    // Send a message to populate the pool, then cleanup
    TcpTransport receiver("127.0.0.1", 0);
    uint16_t recv_port = receiver.local_port();
    std::atomic<bool> got_msg{false};

    std::thread recv_thread([&]() {
        receiver.run([&](const Message& /*msg*/, const std::string& /*addr*/,
                         uint16_t /*port*/) {
            got_msg.store(true);
            receiver.stop();
        });
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    NodeId node_id;
    node_id.id.fill(0x66);
    Message msg;
    msg.type = MessageType::PONG;
    msg.sender = node_id;
    msg.sender_port = transport.local_port();
    msg.payload = {};
    msg.signature = {};
    transport.send("127.0.0.1", recv_port, msg);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!got_msg.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    receiver.stop();
    if (recv_thread.joinable()) {
        recv_thread.join();
    }

    ASSERT_TRUE(got_msg.load());

    // Now there should be a pooled connection; cleanup should not crash
    transport.cleanup_idle_connections();
}

// ---------------------------------------------------------------------------
// All message types serialize/deserialize correctly
// ---------------------------------------------------------------------------

TEST(TcpTransport, AllMessageTypes) {
    const MessageType all_types[] = {
        MessageType::PING,
        MessageType::PONG,
        MessageType::FIND_NODE,
        MessageType::NODES,
        MessageType::STORE,
        MessageType::FIND_VALUE,
        MessageType::VALUE,
        MessageType::SYNC_REQ,
        MessageType::SYNC_RESP,
    };

    NodeId sender;
    sender.id.fill(0x77);

    for (auto type : all_types) {
        Message msg;
        msg.type = type;
        msg.sender = sender;
        msg.sender_port = 9000;
        msg.payload = {static_cast<uint8_t>(type), 0xFF};
        msg.signature = {0x01, 0x02, 0x03};

        auto serialized = serialize_message(msg);
        auto result = deserialize_message(serialized);

        ASSERT_TRUE(result.has_value()) << "Failed for type " << static_cast<int>(type);
        EXPECT_EQ(result->type, type);
        EXPECT_EQ(result->sender, sender);
        EXPECT_EQ(result->sender_port, 9000);
        EXPECT_EQ(result->payload, msg.payload);
        EXPECT_EQ(result->signature, msg.signature);
    }
}

// ---------------------------------------------------------------------------
// Sender port round-trip through header
// ---------------------------------------------------------------------------

TEST(TcpTransport, SenderPortRoundTrip) {
    NodeId sender;
    sender.id.fill(0x11);

    Message msg;
    msg.type = MessageType::PING;
    msg.sender = sender;
    msg.sender_port = 12345;
    msg.payload = {};
    msg.signature = {0x00};

    auto serialized = serialize_message(msg);
    auto result = deserialize_message(serialized);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->sender_port, 12345);
}

// ---------------------------------------------------------------------------
// Encrypted transport test: two nodes communicating over encrypted TCP
// ---------------------------------------------------------------------------

TEST(TcpTransport, EncryptedSendRecv) {
    auto kp_a = generate_keypair();
    auto kp_b = generate_keypair();

    NodeId id_a;
    id_a.id = sha3_256(kp_a.public_key);
    NodeId id_b;
    id_b.id = sha3_256(kp_b.public_key);

    // Node B: receiver
    TcpTransport transport_b("127.0.0.1", 0);
    transport_b.set_signing_keypair(kp_b);
    transport_b.set_node_id(id_b);
    // B needs to look up A's pubkey
    transport_b.set_pubkey_lookup([&](const NodeId& id) -> std::optional<std::vector<uint8_t>> {
        if (id == id_a) return kp_a.public_key;
        return std::nullopt;
    });
    uint16_t port_b = transport_b.local_port();

    std::atomic<bool> received{false};
    std::vector<uint8_t> received_payload;
    std::mutex rx_mutex;

    std::thread recv_thread([&]() {
        transport_b.run([&](const Message& msg, const std::string&, uint16_t) {
            std::lock_guard lock(rx_mutex);
            received_payload = msg.payload;
            received.store(true);
        });
    });

    // Give receiver time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Node A: sender
    TcpTransport transport_a("127.0.0.1", 0);
    transport_a.set_signing_keypair(kp_a);
    transport_a.set_node_id(id_a);
    // A needs to look up B's pubkey
    transport_a.set_pubkey_lookup([&](const NodeId& id) -> std::optional<std::vector<uint8_t>> {
        if (id == id_b) return kp_b.public_key;
        return std::nullopt;
    });

    std::vector<uint8_t> payload = {0xDE, 0xAD, 0xBE, 0xEF};
    Message msg = make_test_message(MessageType::STORE, id_a, payload, transport_a.local_port());
    sign_message(msg, kp_a.secret_key);

    transport_a.send("127.0.0.1", port_b, msg);

    // Wait for message
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!received.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    transport_b.stop();
    recv_thread.join();

    ASSERT_TRUE(received.load()) << "encrypted message not received";
    std::lock_guard lock(rx_mutex);
    EXPECT_EQ(received_payload, payload);
}

// Test that an encrypted node rejects plaintext connections
TEST(TcpTransport, EncryptedReceiverRejectsPlaintext) {
    auto kp_b = generate_keypair();
    NodeId id_b;
    id_b.id = sha3_256(kp_b.public_key);

    // Node B: encrypted receiver
    TcpTransport transport_b("127.0.0.1", 0);
    transport_b.set_signing_keypair(kp_b);
    transport_b.set_node_id(id_b);
    transport_b.set_pubkey_lookup([](const NodeId&) { return std::nullopt; });
    uint16_t port_b = transport_b.local_port();

    std::atomic<bool> received{false};

    std::thread recv_thread([&]() {
        transport_b.run([&](const Message&, const std::string&, uint16_t) {
            received.store(true);
        });
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Node A: plaintext sender (no encryption configured)
    auto kp_a = generate_keypair();
    NodeId id_a;
    id_a.id = sha3_256(kp_a.public_key);

    TcpTransport transport_a("127.0.0.1", 0);
    // No signing keypair set on A — sends plaintext

    std::vector<uint8_t> payload = {0xCA, 0xFE};
    Message msg = make_test_message(MessageType::PING, id_a, payload, transport_a.local_port());

    transport_a.send("127.0.0.1", port_b, msg);

    // Wait briefly — message should NOT be received
    std::this_thread::sleep_for(std::chrono::seconds(1));

    transport_b.stop();
    recv_thread.join();

    EXPECT_FALSE(received.load()) << "plaintext message should be rejected by encrypted node";
}
