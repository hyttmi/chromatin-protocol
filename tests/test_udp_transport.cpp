#include <gtest/gtest.h>

#include "crypto/crypto.h"
#include "kademlia/node_id.h"
#include "kademlia/udp_transport.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

using namespace helix::kademlia;
using namespace helix::crypto;

// ---------------------------------------------------------------------------
// Helper: create a test message with a known sender and payload
// ---------------------------------------------------------------------------

static Message make_test_message(MessageType type, const NodeId& sender,
                                 const std::vector<uint8_t>& payload = {}) {
    Message msg;
    msg.type = type;
    msg.sender = sender;
    msg.payload = payload;
    return msg;
}

// ---------------------------------------------------------------------------
// Serialization round-trip
// ---------------------------------------------------------------------------

TEST(UdpTransport, SerializeDeserializeRoundTrip) {
    NodeId sender;
    sender.id.fill(0xAB);

    std::vector<uint8_t> payload = {0x01, 0x02, 0x03, 0x04, 0x05};
    std::vector<uint8_t> fake_sig = {0xDE, 0xAD, 0xBE, 0xEF};

    Message msg;
    msg.type = MessageType::STORE;
    msg.sender = sender;
    msg.payload = payload;
    msg.signature = fake_sig;

    auto serialized = serialize_message(msg);
    auto result = deserialize_message(serialized);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type, MessageType::STORE);
    EXPECT_EQ(result->sender, sender);
    EXPECT_EQ(result->payload, payload);
    EXPECT_EQ(result->signature, fake_sig);
}

// ---------------------------------------------------------------------------
// Invalid magic
// ---------------------------------------------------------------------------

TEST(UdpTransport, DeserializeInvalidMagic) {
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

TEST(UdpTransport, DeserializeInvalidVersion) {
    NodeId sender;
    sender.id.fill(0x02);

    Message msg = make_test_message(MessageType::PING, sender);
    auto serialized = serialize_message(msg);

    // Corrupt version byte (offset 5)
    serialized[5] = 0xFF;

    auto result = deserialize_message(serialized);
    EXPECT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// Truncated data
// ---------------------------------------------------------------------------

TEST(UdpTransport, DeserializeTruncatedData) {
    // Too short to even contain a header
    std::vector<uint8_t> short_data = {'H', 'E', 'L', 'I', 'X', 0x01, 0x00};
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

TEST(UdpTransport, SignAndVerifyMessage) {
    KeyPair kp = generate_keypair();
    NodeId sender = NodeId::from_pubkey(kp.public_key);

    Message msg = make_test_message(MessageType::FIND_NODE, sender, {0xCA, 0xFE});
    sign_message(msg, kp.secret_key);

    EXPECT_FALSE(msg.signature.empty());
    EXPECT_TRUE(verify_message(msg, kp.public_key));
}

// ---------------------------------------------------------------------------
// Verify rejects tampered payload
// ---------------------------------------------------------------------------

TEST(UdpTransport, VerifyRejectsTamperedPayload) {
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

TEST(UdpTransport, VerifyRejectsWrongKey) {
    KeyPair kp1 = generate_keypair();
    KeyPair kp2 = generate_keypair();
    NodeId sender = NodeId::from_pubkey(kp1.public_key);

    Message msg = make_test_message(MessageType::PING, sender);
    sign_message(msg, kp1.secret_key);

    // Verify with wrong public key
    EXPECT_FALSE(verify_message(msg, kp2.public_key));
}

// ---------------------------------------------------------------------------
// Loopback send/recv test
// ---------------------------------------------------------------------------

TEST(UdpTransport, SendRecvLoopback) {
    UdpTransport transport("127.0.0.1", 0);
    uint16_t port = transport.local_port();
    ASSERT_GT(port, 0);

    NodeId sender;
    sender.id.fill(0x42);
    std::vector<uint8_t> payload = {0x11, 0x22, 0x33};

    Message sent_msg;
    sent_msg.type = MessageType::FIND_VALUE;
    sent_msg.sender = sender;
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

    // Give the recv loop time to start
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
    EXPECT_EQ(received_msg.payload, payload);
    EXPECT_EQ(received_msg.signature, (std::vector<uint8_t>{0xAA, 0xBB}));
    EXPECT_EQ(received_addr, "127.0.0.1");
    EXPECT_GT(received_port, 0);
}

// ---------------------------------------------------------------------------
// All message types serialize/deserialize correctly
// ---------------------------------------------------------------------------

TEST(UdpTransport, AllMessageTypes) {
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
        msg.payload = {static_cast<uint8_t>(type), 0xFF};
        msg.signature = {0x01, 0x02, 0x03};

        auto serialized = serialize_message(msg);
        auto result = deserialize_message(serialized);

        ASSERT_TRUE(result.has_value()) << "Failed for type " << static_cast<int>(type);
        EXPECT_EQ(result->type, type);
        EXPECT_EQ(result->sender, sender);
        EXPECT_EQ(result->payload, msg.payload);
        EXPECT_EQ(result->signature, msg.signature);
    }
}
