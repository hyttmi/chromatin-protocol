#include <gtest/gtest.h>

#include "test_ws_client.h"
#include "ws/ws_server.h"

#include "config/config.h"
#include "crypto/crypto.h"
#include "kademlia/kademlia.h"
#include "kademlia/routing_table.h"
#include "kademlia/tcp_transport.h"
#include "replication/repl_log.h"
#include "storage/storage.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <thread>

#include <json/json.h>

// ---------- hex helpers (test-local) ----------

namespace {

std::string to_hex(std::span<const uint8_t> data) {
    static constexpr char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(data.size() * 2);
    for (uint8_t byte : data) {
        result.push_back(hex_chars[byte >> 4]);
        result.push_back(hex_chars[byte & 0x0F]);
    }
    return result;
}

std::vector<uint8_t> from_hex(const std::string& hex) {
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + c - 'a';
            if (c >= 'A' && c <= 'F') return 10 + c - 'A';
            return -1;
        };
        int h = nibble(hex[i]);
        int l = nibble(hex[i + 1]);
        bytes.push_back(static_cast<uint8_t>((h << 4) | l));
    }
    return bytes;
}

Json::Value parse_json(const std::string& s) {
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream stream(s);
    Json::parseFromStream(builder, stream, &root, &errs);
    return root;
}

// Build a valid GROUP_META binary for testing.
// members: vector of (fingerprint, role) pairs.
// signer_kp: keypair that signs the meta (should be an owner in the list).
std::vector<uint8_t> build_group_meta(
    const chromatin::crypto::Hash& group_id,
    uint32_t version,
    const std::vector<std::pair<chromatin::crypto::Hash, uint8_t>>& members,
    const chromatin::crypto::KeyPair& signer_kp)
{
    std::vector<uint8_t> meta;
    // group_id(32)
    meta.insert(meta.end(), group_id.begin(), group_id.end());
    // owner_fp(32) — first owner in the list
    chromatin::crypto::Hash owner_fp{};
    for (const auto& [fp, role] : members) {
        if (role == 0x02) { owner_fp = fp; break; }
    }
    meta.insert(meta.end(), owner_fp.begin(), owner_fp.end());
    // version(4 BE)
    meta.push_back((version >> 24) & 0xFF);
    meta.push_back((version >> 16) & 0xFF);
    meta.push_back((version >> 8) & 0xFF);
    meta.push_back(version & 0xFF);
    // member_count(2 BE)
    uint16_t count = static_cast<uint16_t>(members.size());
    meta.push_back((count >> 8) & 0xFF);
    meta.push_back(count & 0xFF);
    // per-member: fp(32) + role(1) + kem_ciphertext(1568)
    for (const auto& [fp, role] : members) {
        meta.insert(meta.end(), fp.begin(), fp.end());
        meta.push_back(role);
        meta.resize(meta.size() + 1568, 0x00);  // dummy kem_ciphertext
    }
    // Sign everything so far
    auto signature = chromatin::crypto::sign(meta, signer_kp.secret_key);
    uint16_t sig_len = static_cast<uint16_t>(signature.size());
    meta.push_back((sig_len >> 8) & 0xFF);
    meta.push_back(sig_len & 0xFF);
    meta.insert(meta.end(), signature.begin(), signature.end());
    return meta;
}

std::string send_cmd(TestWsClient& client, const Json::Value& cmd, int timeout_ms = 3000) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    std::string json = Json::writeString(writer, cmd);
    client.send_text(json);
    auto resp = client.recv_text(timeout_ms);
    return resp.value_or("");
}

std::string to_base64(std::span<const uint8_t> data) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        uint32_t triple = (static_cast<uint32_t>(data[i]) << 16) |
                          (static_cast<uint32_t>(data[i + 1]) << 8) |
                          static_cast<uint32_t>(data[i + 2]);
        out.push_back(table[(triple >> 18) & 0x3F]);
        out.push_back(table[(triple >> 12) & 0x3F]);
        out.push_back(table[(triple >> 6) & 0x3F]);
        out.push_back(table[triple & 0x3F]);
    }

    if (i < data.size()) {
        uint32_t triple = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < data.size())
            triple |= static_cast<uint32_t>(data[i + 1]) << 8;

        out.push_back(table[(triple >> 18) & 0x3F]);
        out.push_back(table[(triple >> 12) & 0x3F]);
        if (i + 1 < data.size())
            out.push_back(table[(triple >> 6) & 0x3F]);
        else
            out.push_back('=');
        out.push_back('=');
    }
    return out;
}

} // anonymous namespace

using namespace chromatin;

class WsServerTest : public ::testing::Test {
protected:
    std::filesystem::path db_path_;
    std::unique_ptr<storage::Storage> storage_;
    std::unique_ptr<replication::ReplLog> repl_log_;
    std::unique_ptr<kademlia::RoutingTable> routing_table_;
    std::unique_ptr<kademlia::TcpTransport> transport_;
    std::unique_ptr<kademlia::Kademlia> kademlia_;
    std::unique_ptr<ws::WsServer<false>> server_;
    std::thread tcp_thread_;
    std::thread ws_thread_;
    crypto::KeyPair node_keypair_;
    config::Config cfg_;
    uint16_t ws_port_ = 0;

    void SetUp() override {
        db_path_ = std::filesystem::temp_directory_path() /
                   ("chromatin_ws_test_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(db_path_);

        storage_ = std::make_unique<storage::Storage>(db_path_ / "test.mdbx");
        repl_log_ = std::make_unique<replication::ReplLog>(*storage_);
        routing_table_ = std::make_unique<kademlia::RoutingTable>();

        transport_ = std::make_unique<kademlia::TcpTransport>("127.0.0.1", 0);
        node_keypair_ = crypto::generate_keypair();

        auto node_id = kademlia::NodeId::from_pubkey(node_keypair_.public_key);
        kademlia::NodeInfo self;
        self.id = node_id;
        self.address = "127.0.0.1";
        self.tcp_port = transport_->local_port();
        self.ws_port = 0;  // will be set after WS listen
        self.pubkey = node_keypair_.public_key;
        self.last_seen = std::chrono::steady_clock::now();

        cfg_.bind = "127.0.0.1";
        cfg_.ws_port = 0;  // ephemeral
        cfg_.data_dir = db_path_;
        cfg_.name_pow_difficulty = 8;
        cfg_.contact_pow_difficulty = 8;

        kademlia_ = std::make_unique<kademlia::Kademlia>(
            cfg_, self, *transport_, *routing_table_, *storage_, *repl_log_, node_keypair_);

        // Start TCP accept thread
        tcp_thread_ = std::thread([this]() {
            transport_->run([this](const kademlia::Message& msg,
                                  const std::string& from, uint16_t port) {
                kademlia_->handle_message(msg, from, port);
            });
        });
    }

    // Authenticate helper: does HELLO -> CHALLENGE -> AUTH flow.
    // Returns true if authenticated successfully, false on REDIRECT or failure.
    bool authenticate(TestWsClient& client, const crypto::KeyPair& user_kp) {
        auto fingerprint = crypto::sha3_256(user_kp.public_key);
        std::string fp_hex = to_hex(fingerprint);

        // HELLO
        std::string hello = R"({"type":"HELLO","fingerprint":")" + fp_hex + R"("})";
        if (!client.send_text(hello)) return false;

        auto resp1 = client.recv_text();
        if (!resp1.has_value()) return false;

        auto challenge = parse_json(*resp1);
        if (challenge["type"].asString() != "CHALLENGE") return false;

        // Sign domain-separated data: "chromatin-auth:" || nonce
        std::string nonce_hex = challenge["nonce"].asString();
        auto nonce_bytes = from_hex(nonce_hex);
        if (nonce_bytes.size() != 32) return false;

        const std::string auth_prefix = "chromatin-auth:";
        std::vector<uint8_t> signed_data(auth_prefix.begin(), auth_prefix.end());
        signed_data.insert(signed_data.end(), nonce_bytes.begin(), nonce_bytes.end());
        auto signature = crypto::sign(signed_data, user_kp.secret_key);

        // AUTH
        Json::Value auth_msg;
        auth_msg["type"] = "AUTH";
        auth_msg["id"] = 1;
        auth_msg["signature"] = to_hex(signature);
        auth_msg["pubkey"] = to_hex(user_kp.public_key);

        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        std::string auth_json = Json::writeString(writer, auth_msg);
        if (!client.send_text(auth_json)) return false;

        auto resp2 = client.recv_text();
        if (!resp2.has_value()) return false;

        auto ok = parse_json(*resp2);
        return ok["type"].asString() == "OK";
    }

    void start_ws_server() {
        server_ = std::make_unique<ws::WsServer<false>>(
            cfg_, *kademlia_, *storage_, *repl_log_, node_keypair_);

        kademlia_->set_on_store([this](const crypto::Hash& key, uint8_t type,
                                       std::span<const uint8_t> value) {
            server_->on_kademlia_store(key, type, value);
        });

        ws_thread_ = std::thread([this]() { server_->run(); });

        // Wait for server to start listening
        for (int i = 0; i < 50; ++i) {
            ws_port_ = server_->listening_port();
            if (ws_port_ > 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        ASSERT_GT(ws_port_, 0u) << "WS server failed to start";
    }

    void TearDown() override {
        if (server_) server_->stop();
        if (ws_thread_.joinable()) ws_thread_.join();
        transport_->stop();
        if (tcp_thread_.joinable()) tcp_thread_.join();
        storage_.reset();
        std::filesystem::remove_all(db_path_);
    }
};

TEST_F(WsServerTest, AcceptsConnection) {
    start_ws_server();

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    client.close();
}

TEST_F(WsServerTest, UnknownCommandReturnsError) {
    start_ws_server();

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(client.send_text(R"({"type":"FOOBAR","id":42})"));

    auto resp = client.recv_text();
    ASSERT_TRUE(resp.has_value());

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream s(*resp);
    ASSERT_TRUE(Json::parseFromStream(builder, s, &root, &errs));

    EXPECT_EQ(root["type"].asString(), "ERROR");
    EXPECT_EQ(root["id"].asInt(), 42);
    EXPECT_EQ(root["code"].asInt(), 400);
}

// ---------- Auth flow tests ----------

TEST_F(WsServerTest, HelloChallenge) {
    start_ws_server();

    // Generate a user keypair and compute fingerprint
    auto user_kp = crypto::generate_keypair();
    auto fingerprint = crypto::sha3_256(user_kp.public_key);
    std::string fp_hex = to_hex(fingerprint);

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));

    // Send HELLO
    std::string hello = R"({"type":"HELLO","fingerprint":")" + fp_hex + R"("})";
    ASSERT_TRUE(client.send_text(hello));

    auto resp = client.recv_text();
    ASSERT_TRUE(resp.has_value()) << "no response to HELLO";

    auto root = parse_json(*resp);

    // With a single node, it is always responsible — expect CHALLENGE
    EXPECT_EQ(root["type"].asString(), "CHALLENGE");
    EXPECT_FALSE(root["nonce"].asString().empty());
    EXPECT_EQ(root["nonce"].asString().size(), 64u) << "nonce must be 32 bytes (64 hex chars)";

    client.close();
}

TEST_F(WsServerTest, FullAuthFlow) {
    start_ws_server();

    // Generate a user keypair and compute fingerprint
    auto user_kp = crypto::generate_keypair();
    auto fingerprint = crypto::sha3_256(user_kp.public_key);
    std::string fp_hex = to_hex(fingerprint);

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));

    // Step 1: HELLO
    std::string hello = R"({"type":"HELLO","fingerprint":")" + fp_hex + R"("})";
    ASSERT_TRUE(client.send_text(hello));

    auto resp1 = client.recv_text();
    ASSERT_TRUE(resp1.has_value()) << "no CHALLENGE response";
    auto challenge = parse_json(*resp1);
    ASSERT_EQ(challenge["type"].asString(), "CHALLENGE");

    // Step 2: Sign domain-separated data: "chromatin-auth:" || nonce
    std::string nonce_hex = challenge["nonce"].asString();
    auto nonce_bytes = from_hex(nonce_hex);
    ASSERT_EQ(nonce_bytes.size(), 32u);

    const std::string auth_prefix = "chromatin-auth:";
    std::vector<uint8_t> signed_data(auth_prefix.begin(), auth_prefix.end());
    signed_data.insert(signed_data.end(), nonce_bytes.begin(), nonce_bytes.end());
    auto signature = crypto::sign(signed_data, user_kp.secret_key);
    ASSERT_EQ(signature.size(), crypto::SIGNATURE_SIZE);

    // Step 3: AUTH
    Json::Value auth_msg;
    auth_msg["type"] = "AUTH";
    auth_msg["id"] = 1;
    auth_msg["signature"] = to_hex(signature);
    auth_msg["pubkey"] = to_hex(user_kp.public_key);

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    std::string auth_json = Json::writeString(writer, auth_msg);
    ASSERT_TRUE(client.send_text(auth_json));

    auto resp2 = client.recv_text();
    ASSERT_TRUE(resp2.has_value()) << "no OK response";
    auto ok = parse_json(*resp2);

    EXPECT_EQ(ok["type"].asString(), "OK");
    EXPECT_EQ(ok["id"].asInt(), 1);
    EXPECT_EQ(ok["pending_messages"].asInt(), 0);

    client.close();
}

TEST_F(WsServerTest, CommandBeforeAuthFails) {
    start_ws_server();

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));

    // Try LIST without authenticating
    ASSERT_TRUE(client.send_text(R"({"type":"LIST","id":7})"));

    auto resp = client.recv_text();
    ASSERT_TRUE(resp.has_value()) << "no response to unauthenticated LIST";

    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "ERROR");
    EXPECT_EQ(root["id"].asInt(), 7);
    EXPECT_EQ(root["code"].asInt(), 401);

    client.close();
}

// ---------- LIST tests ----------

TEST_F(WsServerTest, ListEmptyInbox) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    // LIST with no messages in inbox
    ASSERT_TRUE(client.send_text(R"({"type":"LIST","id":10})"));

    auto resp = client.recv_text();
    ASSERT_TRUE(resp.has_value()) << "no response to LIST";

    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "LIST_RESULT");
    EXPECT_EQ(root["id"].asInt(), 10);
    ASSERT_TRUE(root["messages"].isArray());
    EXPECT_EQ(root["messages"].size(), 0u);

    client.close();
}

TEST_F(WsServerTest, ListWithInlineMessage) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();
    auto fingerprint = crypto::sha3_256(user_kp.public_key);

    // Manually insert data into the new two-table model.
    // INDEX key: recipient_fp(32) || msg_id(32) = 64 bytes
    // INDEX value: sender_fp(32) || timestamp(8 BE) || size(4 BE) = 44 bytes

    // Create a fake msg_id (32 bytes)
    crypto::Hash msg_id{};
    msg_id.fill(0xAA);

    // Create a fake sender fingerprint (32 bytes)
    crypto::Hash sender_fp{};
    sender_fp.fill(0xBB);

    // Timestamp
    uint64_t timestamp = 1700000000;

    // Blob data: "Hello"
    std::vector<uint8_t> blob = {0x48, 0x65, 0x6C, 0x6C, 0x6F};
    uint32_t blob_size = static_cast<uint32_t>(blob.size());

    // Build INDEX key: fp(32) || msg_id(32)
    std::vector<uint8_t> index_key;
    index_key.insert(index_key.end(), fingerprint.begin(), fingerprint.end());
    index_key.insert(index_key.end(), msg_id.begin(), msg_id.end());
    ASSERT_EQ(index_key.size(), 64u);

    // Build INDEX value: sender_fp(32) || timestamp(8 BE) || size(4 BE)
    std::vector<uint8_t> index_value;
    index_value.insert(index_value.end(), sender_fp.begin(), sender_fp.end());
    for (int i = 7; i >= 0; --i) {
        index_value.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    }
    index_value.push_back(static_cast<uint8_t>((blob_size >> 24) & 0xFF));
    index_value.push_back(static_cast<uint8_t>((blob_size >> 16) & 0xFF));
    index_value.push_back(static_cast<uint8_t>((blob_size >> 8) & 0xFF));
    index_value.push_back(static_cast<uint8_t>(blob_size & 0xFF));
    ASSERT_EQ(index_value.size(), 44u);

    storage_->put(storage::TABLE_INBOX_INDEX, index_key, index_value);

    // Store blob in TABLE_MESSAGE_BLOBS: key = msg_id, value = raw blob
    std::vector<uint8_t> blob_key(msg_id.begin(), msg_id.end());
    storage_->put(storage::TABLE_MESSAGE_BLOBS, blob_key, blob);

    // Now connect and authenticate
    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    // LIST
    ASSERT_TRUE(client.send_text(R"({"type":"LIST","id":20})"));

    auto resp = client.recv_text();
    ASSERT_TRUE(resp.has_value()) << "no response to LIST";

    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "LIST_RESULT");
    EXPECT_EQ(root["id"].asInt(), 20);
    ASSERT_TRUE(root["messages"].isArray());
    ASSERT_EQ(root["messages"].size(), 1u);

    auto& entry = root["messages"][0];
    EXPECT_EQ(entry["msg_id"].asString(), to_hex(msg_id));
    EXPECT_EQ(entry["from"].asString(), to_hex(sender_fp));
    EXPECT_EQ(entry["timestamp"].asUInt64(), timestamp);
    EXPECT_EQ(entry["size"].asUInt(), blob_size);
    // blob "Hello" -> base64 "SGVsbG8="
    EXPECT_EQ(entry["blob"].asString(), "SGVsbG8=");

    client.close();
}

TEST_F(WsServerTest, ListLargeMessageBlobNull) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();
    auto fingerprint = crypto::sha3_256(user_kp.public_key);

    // Insert an index entry with size > 64 KB (the blob itself is not stored
    // in TABLE_MESSAGE_BLOBS for this test since LIST should not attempt to
    // read it when size > threshold).

    crypto::Hash msg_id{};
    msg_id.fill(0xCC);

    crypto::Hash sender_fp{};
    sender_fp.fill(0xDD);

    uint64_t timestamp = 1700000001;
    uint32_t large_size = 65 * 1024;  // 65 KB > 64 KB threshold

    // Build INDEX key: fp(32) || msg_id(32)
    std::vector<uint8_t> index_key;
    index_key.insert(index_key.end(), fingerprint.begin(), fingerprint.end());
    index_key.insert(index_key.end(), msg_id.begin(), msg_id.end());

    // Build INDEX value: sender_fp(32) || timestamp(8 BE) || size(4 BE)
    std::vector<uint8_t> index_value;
    index_value.insert(index_value.end(), sender_fp.begin(), sender_fp.end());
    for (int i = 7; i >= 0; --i) {
        index_value.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    }
    index_value.push_back(static_cast<uint8_t>((large_size >> 24) & 0xFF));
    index_value.push_back(static_cast<uint8_t>((large_size >> 16) & 0xFF));
    index_value.push_back(static_cast<uint8_t>((large_size >> 8) & 0xFF));
    index_value.push_back(static_cast<uint8_t>(large_size & 0xFF));

    storage_->put(storage::TABLE_INBOX_INDEX, index_key, index_value);

    // Connect and authenticate
    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    // LIST
    ASSERT_TRUE(client.send_text(R"({"type":"LIST","id":30})"));

    auto resp = client.recv_text();
    ASSERT_TRUE(resp.has_value()) << "no response to LIST";

    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "LIST_RESULT");
    EXPECT_EQ(root["id"].asInt(), 30);
    ASSERT_TRUE(root["messages"].isArray());
    ASSERT_EQ(root["messages"].size(), 1u);

    auto& entry = root["messages"][0];
    EXPECT_EQ(entry["msg_id"].asString(), to_hex(msg_id));
    EXPECT_EQ(entry["from"].asString(), to_hex(sender_fp));
    EXPECT_EQ(entry["timestamp"].asUInt64(), timestamp);
    EXPECT_EQ(entry["size"].asUInt(), large_size);
    // Blob should be null for messages larger than 64 KB
    EXPECT_TRUE(entry["blob"].isNull());

    client.close();
}

// ---------- SEND tests ----------

TEST_F(WsServerTest, SendAndList) {
    start_ws_server();

    // Create sender and recipient keypairs
    auto sender_kp = crypto::generate_keypair();
    auto recipient_kp = crypto::generate_keypair();
    auto sender_fp = crypto::sha3_256(sender_kp.public_key);
    auto recipient_fp = crypto::sha3_256(recipient_kp.public_key);

    // Manually add sender to recipient's allowlist.
    // Allowlist key: SHA3-256("allowlist:" || recipient_fp) || sender_fp = 64 bytes
    auto allowlist_prefix = crypto::sha3_256_prefixed("allowlist:", recipient_fp);
    std::vector<uint8_t> allow_key;
    allow_key.reserve(64);
    allow_key.insert(allow_key.end(), allowlist_prefix.begin(), allowlist_prefix.end());
    allow_key.insert(allow_key.end(), sender_fp.begin(), sender_fp.end());
    std::vector<uint8_t> allow_value = {0x01};  // non-empty = allowed
    storage_->put(storage::TABLE_ALLOWLISTS, allow_key, allow_value);

    // Connect and authenticate as sender
    TestWsClient sender_client;
    ASSERT_TRUE(sender_client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(sender_client, sender_kp));

    // SEND a message to recipient
    std::vector<uint8_t> blob_data = {0x48, 0x65, 0x6C, 0x6C, 0x6F};  // "Hello"
    std::string blob_b64 = to_base64(blob_data);

    Json::Value send_msg;
    send_msg["type"] = "SEND";
    send_msg["id"] = 100;
    send_msg["to"] = to_hex(recipient_fp);
    send_msg["blob"] = blob_b64;

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    std::string send_json = Json::writeString(writer, send_msg);
    ASSERT_TRUE(sender_client.send_text(send_json));

    // Use longer timeout (5s) since the store is async via worker pool
    auto send_resp = sender_client.recv_text(5000);
    ASSERT_TRUE(send_resp.has_value()) << "no response to SEND";

    auto send_root = parse_json(*send_resp);
    EXPECT_EQ(send_root["type"].asString(), "SEND_ACK");
    EXPECT_EQ(send_root["id"].asInt(), 100);
    EXPECT_FALSE(send_root["msg_id"].asString().empty());
    EXPECT_EQ(send_root["msg_id"].asString().size(), 64u);  // 32 bytes = 64 hex chars

    sender_client.close();

    // Now connect as recipient and LIST the messages
    TestWsClient recipient_client;
    ASSERT_TRUE(recipient_client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(recipient_client, recipient_kp));

    ASSERT_TRUE(recipient_client.send_text(R"({"type":"LIST","id":200})"));

    auto list_resp = recipient_client.recv_text(5000);
    ASSERT_TRUE(list_resp.has_value()) << "no response to LIST";

    auto list_root = parse_json(*list_resp);
    EXPECT_EQ(list_root["type"].asString(), "LIST_RESULT");
    EXPECT_EQ(list_root["id"].asInt(), 200);
    ASSERT_TRUE(list_root["messages"].isArray());
    ASSERT_EQ(list_root["messages"].size(), 1u);

    auto& entry = list_root["messages"][0];
    EXPECT_EQ(entry["msg_id"].asString(), send_root["msg_id"].asString());
    EXPECT_EQ(entry["from"].asString(), to_hex(sender_fp));
    EXPECT_EQ(entry["size"].asUInt(), static_cast<uint32_t>(blob_data.size()));
    // Small blob should be inlined
    EXPECT_EQ(entry["blob"].asString(), blob_b64);

    recipient_client.close();
}

// ---------- ALLOW / REVOKE tests ----------

TEST_F(WsServerTest, AllowAndRevoke) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();
    auto contact_kp = crypto::generate_keypair();
    auto contact_fp = crypto::sha3_256(contact_kp.public_key);

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    if (!authenticate(client, user_kp)) {
        GTEST_SKIP() << "Node not responsible";
    }

    // Build signature for ALLOW with domain separation:
    // "chromatin:allowlist:" || owner_fp(32) || action(0x01) || allowed_fp(32) || sequence(8 BE)
    auto user_fp_for_sig = crypto::sha3_256(user_kp.public_key);
    const std::string domain = "chromatin:allowlist:";
    std::vector<uint8_t> signed_data;
    signed_data.insert(signed_data.end(), domain.begin(), domain.end());
    signed_data.insert(signed_data.end(), user_fp_for_sig.begin(), user_fp_for_sig.end());
    signed_data.push_back(0x01);  // action = allow
    signed_data.insert(signed_data.end(), contact_fp.begin(), contact_fp.end());
    uint64_t seq = 1;
    for (int i = 7; i >= 0; --i)
        signed_data.push_back(static_cast<uint8_t>((seq >> (i * 8)) & 0xFF));

    auto sig = crypto::sign(signed_data, user_kp.secret_key);

    std::string allow_msg = R"({"type":"ALLOW","id":4,"fingerprint":")" + to_hex(contact_fp) +
                            R"(","sequence":1,"signature":")" + to_hex(sig) + R"("})";
    client.send_text(allow_msg);

    auto resp = client.recv_text(5000);
    ASSERT_TRUE(resp.has_value()) << "no response to ALLOW";
    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "OK");
    EXPECT_EQ(root["id"].asInt(), 4);

    // Verify the entry was stored in TABLE_ALLOWLISTS
    auto user_fp = crypto::sha3_256(user_kp.public_key);
    auto allowlist_key = crypto::sha3_256_prefixed("allowlist:", user_fp);
    std::vector<uint8_t> storage_key;
    storage_key.insert(storage_key.end(), allowlist_key.begin(), allowlist_key.end());
    storage_key.insert(storage_key.end(), contact_fp.begin(), contact_fp.end());

    auto stored = storage_->get(storage::TABLE_ALLOWLISTS, storage_key);
    ASSERT_TRUE(stored.has_value()) << "allowlist entry not found in storage";
    // Stored format: owner_fp(32) || allowed_fp(32) || action(1) || sequence(8 BE) || signature
    ASSERT_GE(stored->size(), 73u);
    EXPECT_EQ((*stored)[64], 0x01);  // action = allow at offset 64

    // Extract stored sequence (bytes 65..72 big-endian)
    uint64_t stored_seq = 0;
    for (int i = 0; i < 8; ++i) {
        stored_seq = (stored_seq << 8) | (*stored)[65 + i];
    }
    EXPECT_EQ(stored_seq, 1u);

    // Now REVOKE the same contact with sequence=2
    // "chromatin:allowlist:" || owner_fp(32) || action(0x00) || allowed_fp(32) || sequence(8 BE)
    std::vector<uint8_t> revoke_data;
    revoke_data.insert(revoke_data.end(), domain.begin(), domain.end());
    revoke_data.insert(revoke_data.end(), user_fp_for_sig.begin(), user_fp_for_sig.end());
    revoke_data.push_back(0x00);  // action = revoke
    revoke_data.insert(revoke_data.end(), contact_fp.begin(), contact_fp.end());
    uint64_t revoke_seq = 2;
    for (int i = 7; i >= 0; --i)
        revoke_data.push_back(static_cast<uint8_t>((revoke_seq >> (i * 8)) & 0xFF));

    auto revoke_sig = crypto::sign(revoke_data, user_kp.secret_key);

    std::string revoke_msg = R"({"type":"REVOKE","id":5,"fingerprint":")" + to_hex(contact_fp) +
                             R"(","sequence":2,"signature":")" + to_hex(revoke_sig) + R"("})";
    client.send_text(revoke_msg);

    auto resp2 = client.recv_text(5000);
    ASSERT_TRUE(resp2.has_value()) << "no response to REVOKE";
    auto root2 = parse_json(*resp2);
    EXPECT_EQ(root2["type"].asString(), "OK");
    EXPECT_EQ(root2["id"].asInt(), 5);

    // Verify the entry was deleted from storage
    auto deleted = storage_->get(storage::TABLE_ALLOWLISTS, storage_key);
    EXPECT_FALSE(deleted.has_value()) << "allowlist entry should be deleted after REVOKE";

    client.close();
}

TEST_F(WsServerTest, AllowRejectsStaleSequence) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();
    auto contact_kp = crypto::generate_keypair();
    auto contact_fp = crypto::sha3_256(contact_kp.public_key);

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    if (!authenticate(client, user_kp)) {
        GTEST_SKIP() << "Node not responsible";
    }

    // First ALLOW with sequence=5 (domain separation)
    auto user_fp = crypto::sha3_256(user_kp.public_key);
    const std::string domain = "chromatin:allowlist:";
    std::vector<uint8_t> signed_data;
    signed_data.insert(signed_data.end(), domain.begin(), domain.end());
    signed_data.insert(signed_data.end(), user_fp.begin(), user_fp.end());
    signed_data.push_back(0x01);
    signed_data.insert(signed_data.end(), contact_fp.begin(), contact_fp.end());
    uint64_t seq = 5;
    for (int i = 7; i >= 0; --i)
        signed_data.push_back(static_cast<uint8_t>((seq >> (i * 8)) & 0xFF));

    auto sig = crypto::sign(signed_data, user_kp.secret_key);

    std::string allow_msg = R"({"type":"ALLOW","id":10,"fingerprint":")" + to_hex(contact_fp) +
                            R"(","sequence":5,"signature":")" + to_hex(sig) + R"("})";
    client.send_text(allow_msg);

    auto resp = client.recv_text(5000);
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(parse_json(*resp)["type"].asString(), "OK");

    // Try ALLOW again with sequence=3 (stale) — should fail
    std::vector<uint8_t> stale_data;
    stale_data.insert(stale_data.end(), domain.begin(), domain.end());
    stale_data.insert(stale_data.end(), user_fp.begin(), user_fp.end());
    stale_data.push_back(0x01);
    stale_data.insert(stale_data.end(), contact_fp.begin(), contact_fp.end());
    uint64_t stale_seq = 3;
    for (int i = 7; i >= 0; --i)
        stale_data.push_back(static_cast<uint8_t>((stale_seq >> (i * 8)) & 0xFF));

    auto stale_sig = crypto::sign(stale_data, user_kp.secret_key);

    std::string stale_msg = R"({"type":"ALLOW","id":11,"fingerprint":")" + to_hex(contact_fp) +
                            R"(","sequence":3,"signature":")" + to_hex(stale_sig) + R"("})";
    client.send_text(stale_msg);

    auto resp2 = client.recv_text(5000);
    ASSERT_TRUE(resp2.has_value());
    auto root2 = parse_json(*resp2);
    EXPECT_EQ(root2["type"].asString(), "ERROR");
    EXPECT_EQ(root2["code"].asInt(), 400);

    client.close();
}

// ---------- CONTACT_REQUEST tests ----------

TEST_F(WsServerTest, ContactRequestWithPoW) {
    start_ws_server();

    auto sender_kp = crypto::generate_keypair();
    auto recipient_kp = crypto::generate_keypair();
    auto sender_fp = crypto::sha3_256(sender_kp.public_key);
    auto recipient_fp = crypto::sha3_256(recipient_kp.public_key);

    // Connect and authenticate as sender
    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, sender_kp));

    // Compute PoW nonce: preimage = "chromatin:request:" || sender_fp || recipient_fp || timestamp(8 BE)
    // Need 16 leading zero bits (~65k attempts avg)
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

    uint64_t pow_nonce = 0;
    for (uint64_t n = 0; n < 10'000'000; ++n) {
        if (crypto::verify_pow(preimage, n, 16)) {
            pow_nonce = n;
            break;
        }
    }
    // Sanity: verify we actually found a valid nonce
    ASSERT_TRUE(crypto::verify_pow(preimage, pow_nonce, 16))
        << "Failed to find valid PoW nonce within 10M iterations";

    // Build CONTACT_REQUEST message
    std::vector<uint8_t> blob_data = {0xDE, 0xAD, 0xBE, 0xEF};
    std::string blob_b64 = to_base64(blob_data);

    Json::Value req;
    req["type"] = "CONTACT_REQUEST";
    req["id"] = 50;
    req["to"] = to_hex(recipient_fp);
    req["blob"] = blob_b64;
    req["pow_nonce"] = Json::UInt64(pow_nonce);
    req["timestamp"] = Json::UInt64(timestamp);

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    std::string req_json = Json::writeString(writer, req);
    ASSERT_TRUE(client.send_text(req_json));

    auto resp = client.recv_text(5000);
    ASSERT_TRUE(resp.has_value()) << "no response to CONTACT_REQUEST";

    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "OK");
    EXPECT_EQ(root["id"].asInt(), 50);

    client.close();
}

TEST_F(WsServerTest, ContactRequestBinaryIncludesBlobLength) {
    start_ws_server();

    auto sender_kp = crypto::generate_keypair();
    auto recipient_kp = crypto::generate_keypair();
    auto sender_fp = crypto::sha3_256(sender_kp.public_key);
    auto recipient_fp = crypto::sha3_256(recipient_kp.public_key);

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, sender_kp));

    // Compute PoW nonce with timestamp
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

    uint64_t pow_nonce = 0;
    for (uint64_t n = 0; n < 10'000'000; ++n) {
        if (crypto::verify_pow(preimage, n, 16)) {
            pow_nonce = n;
            break;
        }
    }
    ASSERT_TRUE(crypto::verify_pow(preimage, pow_nonce, 16));

    std::vector<uint8_t> blob_data = {0xCA, 0xFE, 0xBA, 0xBE};
    std::string blob_b64 = to_base64(blob_data);

    Json::Value req;
    req["type"] = "CONTACT_REQUEST";
    req["id"] = 51;
    req["to"] = to_hex(recipient_fp);
    req["blob"] = blob_b64;
    req["pow_nonce"] = Json::UInt64(pow_nonce);
    req["timestamp"] = Json::UInt64(timestamp);

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    std::string req_json = Json::writeString(writer, req);
    ASSERT_TRUE(client.send_text(req_json));

    auto resp = client.recv_text(5000);
    ASSERT_TRUE(resp.has_value());
    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "OK");

    // Verify stored binary format:
    // recipient_fp(32) || sender_fp(32) || pow_nonce(8 BE) || timestamp(8 BE) || blob_length(4 BE) || blob
    // Storage key is now composite: recipient_fp(32) || sender_fp(32)
    std::vector<uint8_t> storage_key;
    storage_key.insert(storage_key.end(), recipient_fp.begin(), recipient_fp.end());
    storage_key.insert(storage_key.end(), sender_fp.begin(), sender_fp.end());
    auto stored = storage_->get(storage::TABLE_REQUESTS, storage_key);
    ASSERT_TRUE(stored.has_value()) << "contact request should be stored";

    // Minimum: 32 + 32 + 8 + 8 + 4 + 4 = 88 bytes
    ASSERT_GE(stored->size(), 88u);

    // Verify recipient_fp at offset 0
    EXPECT_TRUE(std::equal(recipient_fp.begin(), recipient_fp.end(), stored->begin()));

    // Verify sender_fp at offset 32
    EXPECT_TRUE(std::equal(sender_fp.begin(), sender_fp.end(), stored->begin() + 32));

    // Verify timestamp at offset 72 (8 bytes BE)
    uint64_t stored_ts = 0;
    for (int i = 0; i < 8; ++i) {
        stored_ts = (stored_ts << 8) | (*stored)[72 + i];
    }
    EXPECT_EQ(stored_ts, timestamp);

    // Verify blob_length at offset 80 (4 bytes BE)
    uint32_t stored_blob_len = (static_cast<uint32_t>((*stored)[80]) << 24)
                             | (static_cast<uint32_t>((*stored)[81]) << 16)
                             | (static_cast<uint32_t>((*stored)[82]) << 8)
                             | static_cast<uint32_t>((*stored)[83]);
    EXPECT_EQ(stored_blob_len, 4u) << "blob_length should be 4 (0xCAFEBABE)";

    // Verify blob at offset 84
    EXPECT_EQ(stored->size(), 88u); // 32 + 32 + 8 + 8 + 4 + 4
    std::vector<uint8_t> stored_blob(stored->begin() + 84, stored->end());
    EXPECT_EQ(stored_blob, blob_data);

    client.close();
}

// ---------- GET tests ----------

TEST_F(WsServerTest, GetSmallBlob) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();
    auto fingerprint = crypto::sha3_256(user_kp.public_key);

    // Create a fake msg_id (32 bytes) and blob
    crypto::Hash msg_id{};
    msg_id.fill(0xEE);
    std::vector<uint8_t> blob = {0x48, 0x65, 0x6C, 0x6C, 0x6F};  // "Hello"

    // Store inbox index entry (required for authorization)
    crypto::Hash sender_fp{};
    sender_fp.fill(0xBB);
    uint64_t timestamp = 1700000000;
    uint32_t blob_size = static_cast<uint32_t>(blob.size());

    std::vector<uint8_t> index_key;
    index_key.insert(index_key.end(), fingerprint.begin(), fingerprint.end());
    index_key.insert(index_key.end(), msg_id.begin(), msg_id.end());

    std::vector<uint8_t> index_value;
    index_value.insert(index_value.end(), sender_fp.begin(), sender_fp.end());
    for (int i = 7; i >= 0; --i)
        index_value.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    index_value.push_back(static_cast<uint8_t>((blob_size >> 24) & 0xFF));
    index_value.push_back(static_cast<uint8_t>((blob_size >> 16) & 0xFF));
    index_value.push_back(static_cast<uint8_t>((blob_size >> 8) & 0xFF));
    index_value.push_back(static_cast<uint8_t>(blob_size & 0xFF));

    storage_->put(storage::TABLE_INBOX_INDEX, index_key, index_value);

    // Store the blob in TABLE_MESSAGE_BLOBS
    std::vector<uint8_t> blob_key(msg_id.begin(), msg_id.end());
    storage_->put(storage::TABLE_MESSAGE_BLOBS, blob_key, blob);

    // Connect and authenticate
    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    // Send GET with the msg_id
    Json::Value get_msg;
    get_msg["type"] = "GET";
    get_msg["id"] = 40;
    get_msg["msg_id"] = to_hex(msg_id);

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    std::string get_json = Json::writeString(writer, get_msg);
    ASSERT_TRUE(client.send_text(get_json));

    auto resp = client.recv_text();
    ASSERT_TRUE(resp.has_value()) << "no response to GET";

    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "GET_RESULT");
    EXPECT_EQ(root["id"].asInt(), 40);
    EXPECT_EQ(root["msg_id"].asString(), to_hex(msg_id));
    // blob "Hello" -> base64 "SGVsbG8="
    EXPECT_EQ(root["blob"].asString(), "SGVsbG8=");

    client.close();
}

TEST_F(WsServerTest, GetNotFound) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();

    // Connect and authenticate
    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    // Send GET with a non-existent msg_id
    std::string fake_msg_id(64, 'f');  // 64 hex chars of 'f'

    Json::Value get_msg;
    get_msg["type"] = "GET";
    get_msg["id"] = 41;
    get_msg["msg_id"] = fake_msg_id;

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    std::string get_json = Json::writeString(writer, get_msg);
    ASSERT_TRUE(client.send_text(get_json));

    auto resp = client.recv_text();
    ASSERT_TRUE(resp.has_value()) << "no response to GET";

    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "ERROR");
    EXPECT_EQ(root["id"].asInt(), 41);
    EXPECT_EQ(root["code"].asInt(), 404);

    client.close();
}

// ---------- DELETE tests ----------

TEST_F(WsServerTest, DeleteMessage) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();
    auto fingerprint = crypto::sha3_256(user_kp.public_key);

    // Store a message in both tables
    crypto::Hash msg_id{};
    msg_id.fill(0xDE);
    crypto::Hash sender_fp{};
    sender_fp.fill(0xAD);
    uint64_t timestamp = 1700000500;
    std::vector<uint8_t> blob = {0x01, 0x02, 0x03};

    // INDEX key: recipient_fp(32) || msg_id(32)
    std::vector<uint8_t> idx_key;
    idx_key.insert(idx_key.end(), fingerprint.begin(), fingerprint.end());
    idx_key.insert(idx_key.end(), msg_id.begin(), msg_id.end());
    // INDEX value: sender_fp(32) || timestamp(8 BE) || size(4 BE)
    std::vector<uint8_t> idx_value;
    idx_value.insert(idx_value.end(), sender_fp.begin(), sender_fp.end());
    for (int i = 7; i >= 0; --i)
        idx_value.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    uint32_t sz = static_cast<uint32_t>(blob.size());
    idx_value.push_back(static_cast<uint8_t>((sz >> 24) & 0xFF));
    idx_value.push_back(static_cast<uint8_t>((sz >> 16) & 0xFF));
    idx_value.push_back(static_cast<uint8_t>((sz >> 8) & 0xFF));
    idx_value.push_back(static_cast<uint8_t>(sz & 0xFF));
    storage_->put(storage::TABLE_INBOX_INDEX, idx_key, idx_value);

    // BLOB key: msg_id(32), value: raw blob
    std::vector<uint8_t> blob_key(msg_id.begin(), msg_id.end());
    storage_->put(storage::TABLE_MESSAGE_BLOBS, blob_key, blob);

    // Connect and authenticate
    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    // DELETE the message
    Json::Value del;
    del["type"] = "DELETE";
    del["id"] = 60;
    Json::Value ids(Json::arrayValue);
    ids.append(to_hex(msg_id));
    del["msg_ids"] = ids;

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    ASSERT_TRUE(client.send_text(Json::writeString(writer, del)));

    auto resp = client.recv_text(3000);
    ASSERT_TRUE(resp.has_value());

    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "OK");
    EXPECT_EQ(root["id"].asInt(), 60);

    // Verify both tables are cleaned up
    EXPECT_FALSE(storage_->get(storage::TABLE_INBOX_INDEX, idx_key).has_value());
    EXPECT_FALSE(storage_->get(storage::TABLE_MESSAGE_BLOBS, blob_key).has_value());

    client.close();
}

// ---------- Push notification tests ----------

TEST_F(WsServerTest, PushNotification) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();
    auto user_fp = crypto::sha3_256(user_kp.public_key);

    // Connect and authenticate
    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    // Build a fake inbox message binary (Kademlia STORE value):
    // recipient_fp(32) || msg_id(32) || sender_fp(32) || timestamp(8 BE) || blob_len(4 BE) || blob
    crypto::Hash msg_id{};
    msg_id.fill(0xCC);
    crypto::Hash sender_fp{};
    sender_fp.fill(0xDD);
    uint64_t timestamp = 1700000042;
    std::vector<uint8_t> blob = {0x48, 0x65, 0x6C, 0x6C, 0x6F};  // "Hello"

    std::vector<uint8_t> msg_binary;
    msg_binary.insert(msg_binary.end(), user_fp.begin(), user_fp.end());  // recipient_fp
    msg_binary.insert(msg_binary.end(), msg_id.begin(), msg_id.end());
    msg_binary.insert(msg_binary.end(), sender_fp.begin(), sender_fp.end());
    for (int i = 7; i >= 0; --i) {
        msg_binary.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    }
    uint32_t blob_len = static_cast<uint32_t>(blob.size());
    msg_binary.push_back(static_cast<uint8_t>((blob_len >> 24) & 0xFF));
    msg_binary.push_back(static_cast<uint8_t>((blob_len >> 16) & 0xFF));
    msg_binary.push_back(static_cast<uint8_t>((blob_len >> 8) & 0xFF));
    msg_binary.push_back(static_cast<uint8_t>(blob_len & 0xFF));
    msg_binary.insert(msg_binary.end(), blob.begin(), blob.end());

    // Compute inbox_key = SHA3-256("inbox:" || user_fp)
    auto inbox_key = crypto::sha3_256_prefixed("inbox:", user_fp);

    // Simulate a Kademlia STORE arriving for this user's inbox
    server_->on_kademlia_store(inbox_key, 0x02, msg_binary);

    // The push should arrive as a NEW_MESSAGE on the WebSocket
    auto resp = client.recv_text(3000);
    ASSERT_TRUE(resp.has_value()) << "no push notification received";

    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "NEW_MESSAGE");
    EXPECT_EQ(root["msg_id"].asString(), to_hex(msg_id));
    EXPECT_EQ(root["from"].asString(), to_hex(sender_fp));
    EXPECT_EQ(root["timestamp"].asUInt64(), timestamp);
    EXPECT_EQ(root["size"].asUInt(), blob.size());
    EXPECT_EQ(root["blob"].asString(), "SGVsbG8=");  // base64("Hello")

    client.close();
}

TEST_F(WsServerTest, PushLargeMessageBlobNull) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();
    auto user_fp = crypto::sha3_256(user_kp.public_key);

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    // Build inbox message with blob > INLINE_THRESHOLD (64 KB)
    // recipient_fp(32) || msg_id(32) || sender_fp(32) || timestamp(8 BE) || blob_len(4 BE) || blob
    crypto::Hash msg_id{};
    msg_id.fill(0xAA);
    crypto::Hash sender_fp{};
    sender_fp.fill(0xBB);
    uint64_t timestamp = 1700000099;
    std::vector<uint8_t> blob(70000, 0x42);  // 70 KB > 64 KB threshold

    std::vector<uint8_t> msg_binary;
    msg_binary.insert(msg_binary.end(), user_fp.begin(), user_fp.end());  // recipient_fp
    msg_binary.insert(msg_binary.end(), msg_id.begin(), msg_id.end());
    msg_binary.insert(msg_binary.end(), sender_fp.begin(), sender_fp.end());
    for (int i = 7; i >= 0; --i) {
        msg_binary.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    }
    uint32_t blob_len = static_cast<uint32_t>(blob.size());
    msg_binary.push_back(static_cast<uint8_t>((blob_len >> 24) & 0xFF));
    msg_binary.push_back(static_cast<uint8_t>((blob_len >> 16) & 0xFF));
    msg_binary.push_back(static_cast<uint8_t>((blob_len >> 8) & 0xFF));
    msg_binary.push_back(static_cast<uint8_t>(blob_len & 0xFF));
    msg_binary.insert(msg_binary.end(), blob.begin(), blob.end());

    auto inbox_key = crypto::sha3_256_prefixed("inbox:", user_fp);
    server_->on_kademlia_store(inbox_key, 0x02, msg_binary);

    auto resp = client.recv_text(3000);
    ASSERT_TRUE(resp.has_value()) << "no push notification received";

    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "NEW_MESSAGE");
    EXPECT_EQ(root["msg_id"].asString(), to_hex(msg_id));
    EXPECT_EQ(root["from"].asString(), to_hex(sender_fp));
    EXPECT_EQ(root["timestamp"].asUInt64(), timestamp);
    EXPECT_EQ(root["size"].asUInt(), 70000u);
    EXPECT_TRUE(root["blob"].isNull()) << "large blob should be null, not inlined";

    client.close();
}

// ---------- Binary frame tests ----------

TEST_F(WsServerTest, BinaryFrameWithoutUploadReturnsError) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    // Send a binary frame with no active upload — should get error
    std::vector<uint8_t> binary_frame = {
        0x01,                    // frame_type = UPLOAD_CHUNK
        0x00, 0x00, 0x00, 0x01, // request_id = 1
        0x00, 0x00,              // chunk_index = 0
        0xDE, 0xAD              // payload
    };
    ASSERT_TRUE(client.send_binary(binary_frame));

    auto resp = client.recv_text(3000);
    ASSERT_TRUE(resp.has_value());

    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "ERROR");
    EXPECT_EQ(root["code"].asInt(), 400);

    client.close();
}

TEST_F(WsServerTest, SendLargeChunked) {
    start_ws_server();

    // Create sender and recipient keypairs
    auto sender_kp = crypto::generate_keypair();
    auto recipient_kp = crypto::generate_keypair();
    auto sender_fp = crypto::sha3_256(sender_kp.public_key);
    auto recipient_fp = crypto::sha3_256(recipient_kp.public_key);

    // Manually add sender to recipient's allowlist
    // Key format: SHA3-256("allowlist:" || recipient_fp) || sender_fp
    auto allowlist_key = crypto::sha3_256_prefixed("allowlist:", recipient_fp);
    std::vector<uint8_t> allow_key;
    allow_key.insert(allow_key.end(), allowlist_key.begin(), allowlist_key.end());
    allow_key.insert(allow_key.end(), sender_fp.begin(), sender_fp.end());
    std::vector<uint8_t> allow_value = {0x01};
    storage_->put(storage::TABLE_ALLOWLISTS, allow_key, allow_value);

    // Connect and authenticate as sender
    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, sender_kp));

    // Prepare a 70 KB blob
    const size_t blob_size = 70 * 1024;
    std::vector<uint8_t> blob_data(blob_size);
    for (size_t i = 0; i < blob_size; ++i) {
        blob_data[i] = static_cast<uint8_t>(i & 0xFF);
    }

    // Send SEND with "size" field (no "blob") to initiate chunked upload
    Json::Value send_msg;
    send_msg["type"] = "SEND";
    send_msg["id"] = 300;
    send_msg["to"] = to_hex(recipient_fp);
    send_msg["size"] = Json::UInt64(blob_size);

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    std::string send_json_str = Json::writeString(writer, send_msg);
    ASSERT_TRUE(client.send_text(send_json_str));

    // Expect SEND_READY with request_id
    auto ready_resp = client.recv_text(5000);
    ASSERT_TRUE(ready_resp.has_value()) << "no SEND_READY response";

    auto ready_root = parse_json(*ready_resp);
    EXPECT_EQ(ready_root["type"].asString(), "SEND_READY");
    EXPECT_EQ(ready_root["id"].asInt(), 300);
    ASSERT_TRUE(ready_root.isMember("request_id"));
    uint32_t request_id = ready_root["request_id"].asUInt();

    // Send the blob as a single UPLOAD_CHUNK binary frame (70 KB < 1 MiB)
    // Binary frame header: frame_type(1) + request_id(4) + chunk_index(2)
    std::vector<uint8_t> binary_frame;
    binary_frame.reserve(7 + blob_size);
    binary_frame.push_back(0x01);  // frame_type = UPLOAD_CHUNK
    binary_frame.push_back(static_cast<uint8_t>((request_id >> 24) & 0xFF));
    binary_frame.push_back(static_cast<uint8_t>((request_id >> 16) & 0xFF));
    binary_frame.push_back(static_cast<uint8_t>((request_id >> 8) & 0xFF));
    binary_frame.push_back(static_cast<uint8_t>(request_id & 0xFF));
    binary_frame.push_back(0x00);  // chunk_index high byte
    binary_frame.push_back(0x00);  // chunk_index low byte
    binary_frame.insert(binary_frame.end(), blob_data.begin(), blob_data.end());
    ASSERT_TRUE(client.send_binary(binary_frame));

    // Expect SEND_ACK with msg_id
    auto ack_resp = client.recv_text(5000);
    ASSERT_TRUE(ack_resp.has_value()) << "no SEND_ACK response";

    auto ack_root = parse_json(*ack_resp);
    EXPECT_EQ(ack_root["type"].asString(), "SEND_ACK");
    EXPECT_EQ(ack_root["id"].asInt(), 300);
    EXPECT_FALSE(ack_root["msg_id"].asString().empty());
    EXPECT_EQ(ack_root["msg_id"].asString().size(), 64u);  // 32 bytes = 64 hex chars

    // Verify blob is stored in TABLE_MESSAGE_BLOBS
    auto msg_id_bytes = from_hex(ack_root["msg_id"].asString());
    ASSERT_EQ(msg_id_bytes.size(), 32u);

    auto stored_blob = storage_->get(storage::TABLE_MESSAGE_BLOBS, msg_id_bytes);
    ASSERT_TRUE(stored_blob.has_value()) << "blob not found in TABLE_MESSAGE_BLOBS";
    EXPECT_EQ(stored_blob->size(), blob_size);
    EXPECT_EQ(*stored_blob, blob_data);

    client.close();
}

TEST_F(WsServerTest, GetLargeChunked) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();
    auto fingerprint = crypto::sha3_256(user_kp.public_key);

    // Store a 70 KB blob (> 64 KB threshold)
    crypto::Hash msg_id{};
    msg_id.fill(0xFA);
    std::vector<uint8_t> blob(70000, 0x42);  // 70 KB of 'B'

    // Store inbox index entry (needed for auth check)
    crypto::Hash sender_fp{};
    sender_fp.fill(0xBB);
    uint64_t timestamp = 1700000000;
    uint32_t blob_size = static_cast<uint32_t>(blob.size());

    std::vector<uint8_t> index_key;
    index_key.insert(index_key.end(), fingerprint.begin(), fingerprint.end());
    index_key.insert(index_key.end(), msg_id.begin(), msg_id.end());

    std::vector<uint8_t> index_value;
    index_value.insert(index_value.end(), sender_fp.begin(), sender_fp.end());
    for (int i = 7; i >= 0; --i)
        index_value.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    index_value.push_back(static_cast<uint8_t>((blob_size >> 24) & 0xFF));
    index_value.push_back(static_cast<uint8_t>((blob_size >> 16) & 0xFF));
    index_value.push_back(static_cast<uint8_t>((blob_size >> 8) & 0xFF));
    index_value.push_back(static_cast<uint8_t>(blob_size & 0xFF));

    storage_->put(storage::TABLE_INBOX_INDEX, index_key, index_value);

    // Store blob in TABLE_MESSAGE_BLOBS
    std::vector<uint8_t> blob_key(msg_id.begin(), msg_id.end());
    storage_->put(storage::TABLE_MESSAGE_BLOBS, blob_key, blob);

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    Json::Value get_msg;
    get_msg["type"] = "GET";
    get_msg["id"] = 50;
    get_msg["msg_id"] = to_hex(msg_id);

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    ASSERT_TRUE(client.send_text(Json::writeString(writer, get_msg)));

    // First: JSON header
    auto header_resp = client.recv_text(3000);
    ASSERT_TRUE(header_resp.has_value());

    auto header = parse_json(*header_resp);
    EXPECT_EQ(header["type"].asString(), "GET_RESULT");
    EXPECT_EQ(header["id"].asInt(), 50);
    EXPECT_EQ(header["msg_id"].asString(), to_hex(msg_id));
    EXPECT_EQ(header["size"].asUInt(), blob.size());
    uint32_t expected_chunks = (blob.size() + 1048575) / 1048576;
    EXPECT_EQ(header["chunks"].asUInt(), expected_chunks);

    // Then: binary DOWNLOAD_CHUNK frames
    std::vector<uint8_t> received_data;
    for (uint32_t i = 0; i < expected_chunks; ++i) {
        auto frame = client.recv_frame(3000);
        ASSERT_TRUE(frame.has_value()) << "missing chunk " << i;
        ASSERT_EQ(frame->opcode, 0x02) << "chunk must be binary frame";

        // Parse header: [1B type][4B request_id][2B chunk_index]
        ASSERT_GE(frame->data.size(), 7u);
        EXPECT_EQ(frame->data[0], 0x02);  // DOWNLOAD_CHUNK
        uint16_t cidx = (static_cast<uint16_t>(frame->data[5]) << 8) | frame->data[6];
        EXPECT_EQ(cidx, i);

        auto payload_start = frame->data.data() + 7;
        auto payload_size = frame->data.size() - 7;
        received_data.insert(received_data.end(), payload_start, payload_start + payload_size);
    }

    EXPECT_EQ(received_data.size(), blob.size());
    EXPECT_EQ(received_data, blob);

    client.close();
}

TEST_F(WsServerTest, SendTooLargeRejects) {
    start_ws_server();

    auto sender_kp = crypto::generate_keypair();
    auto recipient_kp = crypto::generate_keypair();
    auto recipient_fp = crypto::sha3_256(recipient_kp.public_key);

    // Connect and authenticate
    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, sender_kp));

    // Send SEND with size > 50 MiB
    Json::Value send_msg;
    send_msg["type"] = "SEND";
    send_msg["id"] = 400;
    send_msg["to"] = to_hex(recipient_fp);
    send_msg["size"] = Json::UInt64(51ULL * 1024 * 1024);  // 51 MiB > 50 MiB limit

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    std::string send_json_str = Json::writeString(writer, send_msg);
    ASSERT_TRUE(client.send_text(send_json_str));

    auto resp = client.recv_text(5000);
    ASSERT_TRUE(resp.has_value()) << "no response to oversized SEND";

    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "ERROR");
    EXPECT_EQ(root["code"].asInt(), 413);
    EXPECT_EQ(root["reason"].asString(), "attachment too large");

    client.close();
}

TEST_F(WsServerTest, UploadAlreadyInProgressRejects) {
    start_ws_server();

    auto sender_kp = crypto::generate_keypair();
    auto recipient_kp = crypto::generate_keypair();
    auto sender_fp = crypto::sha3_256(sender_kp.public_key);
    auto recipient_fp = crypto::sha3_256(recipient_kp.public_key);

    // Add sender to recipient's allowlist
    auto allowlist_key = crypto::sha3_256_prefixed("allowlist:", recipient_fp);
    std::vector<uint8_t> allow_key;
    allow_key.insert(allow_key.end(), allowlist_key.begin(), allowlist_key.end());
    allow_key.insert(allow_key.end(), sender_fp.begin(), sender_fp.end());
    std::vector<uint8_t> allow_value = {0x01};
    storage_->put(storage::TABLE_ALLOWLISTS, allow_key, allow_value);

    // Connect and authenticate
    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, sender_kp));

    // First large SEND (no blob, just size) — should get SEND_READY
    Json::Value send1;
    send1["type"] = "SEND";
    send1["id"] = 500;
    send1["to"] = to_hex(recipient_fp);
    send1["size"] = Json::UInt64(100 * 1024);  // 100 KB

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    ASSERT_TRUE(client.send_text(Json::writeString(writer, send1)));

    auto resp1 = client.recv_text(5000);
    ASSERT_TRUE(resp1.has_value()) << "no SEND_READY response";
    auto root1 = parse_json(*resp1);
    EXPECT_EQ(root1["type"].asString(), "SEND_READY");

    // Second large SEND while first is still pending — should get 429
    Json::Value send2;
    send2["type"] = "SEND";
    send2["id"] = 501;
    send2["to"] = to_hex(recipient_fp);
    send2["size"] = Json::UInt64(100 * 1024);

    ASSERT_TRUE(client.send_text(Json::writeString(writer, send2)));

    auto resp2 = client.recv_text(5000);
    ASSERT_TRUE(resp2.has_value()) << "no response to second SEND";
    auto root2 = parse_json(*resp2);
    EXPECT_EQ(root2["type"].asString(), "ERROR");
    EXPECT_EQ(root2["id"].asInt(), 501);
    EXPECT_EQ(root2["code"].asInt(), 429);
    EXPECT_EQ(root2["reason"].asString(), "upload already in progress");

    client.close();
}

// ---------- DELETE replication test ----------

TEST_F(WsServerTest, DeleteReplicates) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();
    auto user_fp = crypto::sha3_256(user_kp.public_key);

    // Pre-seed an inbox message directly in storage
    crypto::Hash msg_id{};
    msg_id[0] = 0xDD;
    std::vector<uint8_t> idx_key;
    idx_key.insert(idx_key.end(), user_fp.begin(), user_fp.end());
    idx_key.insert(idx_key.end(), msg_id.begin(), msg_id.end());
    std::vector<uint8_t> idx_val(44, 0x01);
    storage_->put(storage::TABLE_INBOX_INDEX, idx_key, idx_val);
    std::vector<uint8_t> blob_data = {0xDE, 0xAD};
    storage_->put(storage::TABLE_MESSAGE_BLOBS, msg_id, blob_data);

    // Connect and authenticate
    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    // Send DELETE command
    Json::Value del_msg;
    del_msg["type"] = "DELETE";
    del_msg["id"] = 99;
    del_msg["msg_ids"] = Json::Value(Json::arrayValue);
    del_msg["msg_ids"].append(to_hex(msg_id));

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    ASSERT_TRUE(client.send_text(Json::writeString(writer, del_msg)));

    auto resp = client.recv_text(5000);
    ASSERT_TRUE(resp.has_value()) << "no DELETE response";
    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "OK");
    EXPECT_EQ(root["id"].asInt(), 99);

    // Give worker pool time to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Verify local storage is deleted
    EXPECT_FALSE(storage_->get(storage::TABLE_INBOX_INDEX, idx_key).has_value())
        << "inbox index should be deleted";
    EXPECT_FALSE(storage_->get(storage::TABLE_MESSAGE_BLOBS, msg_id).has_value())
        << "blob should be deleted";

    // Verify repl_log has a DEL entry
    auto inbox_key = crypto::sha3_256_prefixed("inbox:", user_fp);
    auto entries = repl_log_->entries_after(inbox_key, 0);
    bool found_del = false;
    for (const auto& entry : entries) {
        if (entry.op == replication::Op::DEL && entry.data_type == 0x02) {
            found_del = true;
            break;
        }
    }
    EXPECT_TRUE(found_del) << "repl_log should have DEL entry for inbox delete";

    client.close();
}

// ---------- Rate limiting test ----------

TEST_F(WsServerTest, RateLimitExceeded) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    // Rapid-fire LIST commands to exhaust the token bucket (50 tokens, 1 per LIST)
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    int rate_limited_count = 0;

    for (int i = 0; i < 60; ++i) {
        Json::Value list_msg;
        list_msg["type"] = "LIST";
        list_msg["id"] = 1000 + i;
        ASSERT_TRUE(client.send_text(Json::writeString(writer, list_msg)));

        auto resp = client.recv_text(2000);
        ASSERT_TRUE(resp.has_value());
        auto root = parse_json(*resp);
        if (root["code"].asInt() == 429) {
            rate_limited_count++;
        }
    }

    EXPECT_GT(rate_limited_count, 0) << "Should have hit rate limit after 50+ rapid commands";

    client.close();
}

TEST_F(WsServerTest, RateLimitSharedAcrossConnections) {
    // Use a small token bucket so we can easily exhaust it with two connections
    cfg_.rate_limit_tokens = 10.0;
    cfg_.rate_limit_max = 10.0;
    cfg_.rate_limit_refill = 0.0;  // no refill — makes test deterministic
    start_ws_server();

    // Same identity for both connections
    auto user_kp = crypto::generate_keypair();

    // Connect and authenticate client 1
    TestWsClient client1;
    ASSERT_TRUE(client1.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client1, user_kp));

    // Connect and authenticate client 2 (same identity)
    TestWsClient client2;
    ASSERT_TRUE(client2.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client2, user_kp));

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";

    // Send 7 requests from client 1 — consumes 7 of 10 tokens from the shared pool
    // (AUTH consumed 1 token from each connection, but those use the per-session
    //  rate limiter for pre-auth, so the fingerprint pool is still at 10 tokens.
    //  Actually AUTH consumes from the shared pool after successful auth added it.)
    // After both auths: the fp limiter was initialized once at 10 tokens.
    // AUTH itself costs 1.0 each, but AUTH runs before the session is marked
    // authenticated, so the pre-auth per-session limiter handles it.
    // Post-auth commands consume from the shared fp limiter.

    for (int i = 0; i < 7; ++i) {
        Json::Value list_msg;
        list_msg["type"] = "LIST";
        list_msg["id"] = 3000 + i;
        ASSERT_TRUE(client1.send_text(Json::writeString(writer, list_msg)));

        auto resp = client1.recv_text(2000);
        ASSERT_TRUE(resp.has_value());
        auto root = parse_json(*resp);
        // These should all succeed (7 of 10 tokens)
        EXPECT_NE(root["code"].asInt(), 429) << "client1 request " << i << " should not be rate limited";
    }

    // Now send requests from client 2 — only 3 tokens remain in shared pool
    int rate_limited_count = 0;
    for (int i = 0; i < 6; ++i) {
        Json::Value list_msg;
        list_msg["type"] = "LIST";
        list_msg["id"] = 4000 + i;
        ASSERT_TRUE(client2.send_text(Json::writeString(writer, list_msg)));

        auto resp = client2.recv_text(2000);
        ASSERT_TRUE(resp.has_value());
        auto root = parse_json(*resp);
        if (root["code"].asInt() == 429) {
            rate_limited_count++;
        }
    }

    // With only 3 tokens left, at least 3 of the 6 requests from client2 should be rejected
    EXPECT_GE(rate_limited_count, 3)
        << "Same-fingerprint connections should share the rate limit pool";

    client1.close();
    client2.close();
}

TEST_F(WsServerTest, HelloRateLimited) {
    start_ws_server();

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));

    // Generate a fingerprint for HELLO
    auto user_kp = crypto::generate_keypair();
    auto fingerprint = crypto::sha3_256(user_kp.public_key);
    std::string fp_hex = to_hex(fingerprint);

    // Rapid-fire HELLO commands to exhaust the token bucket (50 tokens, 1 per HELLO)
    int rate_limited_count = 0;

    for (int i = 0; i < 60; ++i) {
        std::string hello = R"({"type":"HELLO","id":)" + std::to_string(2000 + i) +
                            R"(,"fingerprint":")" + fp_hex + R"("})";
        ASSERT_TRUE(client.send_text(hello));

        auto resp = client.recv_text(2000);
        ASSERT_TRUE(resp.has_value());
        auto root = parse_json(*resp);
        if (root["code"].asInt() == 429) {
            rate_limited_count++;
        }
    }

    EXPECT_GT(rate_limited_count, 0) << "HELLO should be rate limited after 50+ rapid commands";

    client.close();
}

// ---------- STATUS (no auth required) ----------

TEST_F(WsServerTest, StatusWithoutAuth) {
    start_ws_server();

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));

    // STATUS should work without authentication
    ASSERT_TRUE(client.send_text(R"({"type":"STATUS","id":1})"));
    auto resp = client.recv_text(2000);
    ASSERT_TRUE(resp.has_value());

    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "STATUS_RESP");
    EXPECT_EQ(root["id"].asInt(), 1);
    EXPECT_TRUE(root.isMember("node_id"));
    EXPECT_TRUE(root.isMember("uptime_seconds"));
    EXPECT_TRUE(root.isMember("connected_clients"));
    EXPECT_TRUE(root.isMember("authenticated_clients"));
    EXPECT_TRUE(root.isMember("routing_table_size"));
    EXPECT_GE(root["uptime_seconds"].asInt64(), 0);
    EXPECT_GE(root["connected_clients"].asUInt64(), 1u);  // at least this client
    EXPECT_EQ(root["authenticated_clients"].asUInt64(), 0u);  // not authenticated

    client.close();
}

// ---------- Multi-device push notification test ----------

TEST_F(WsServerTest, MultiDevicePush) {
    start_ws_server();

    // Single identity, two devices
    auto user_kp = crypto::generate_keypair();
    auto user_fp = crypto::sha3_256(user_kp.public_key);

    // Connect and authenticate device 1
    TestWsClient device1;
    ASSERT_TRUE(device1.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(device1, user_kp));

    // Connect and authenticate device 2 (same identity)
    TestWsClient device2;
    ASSERT_TRUE(device2.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(device2, user_kp));

    // Build a fake inbox message binary (Kademlia STORE value):
    // recipient_fp(32) || msg_id(32) || sender_fp(32) || timestamp(8 BE) || blob_len(4 BE) || blob
    crypto::Hash msg_id{};
    msg_id.fill(0xF1);
    crypto::Hash sender_fp{};
    sender_fp.fill(0xF2);
    uint64_t timestamp = 1700000123;
    std::vector<uint8_t> blob = {0xCA, 0xFE};

    std::vector<uint8_t> msg_binary;
    msg_binary.insert(msg_binary.end(), user_fp.begin(), user_fp.end());  // recipient_fp
    msg_binary.insert(msg_binary.end(), msg_id.begin(), msg_id.end());
    msg_binary.insert(msg_binary.end(), sender_fp.begin(), sender_fp.end());
    for (int i = 7; i >= 0; --i) {
        msg_binary.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    }
    uint32_t blob_len = static_cast<uint32_t>(blob.size());
    msg_binary.push_back(static_cast<uint8_t>((blob_len >> 24) & 0xFF));
    msg_binary.push_back(static_cast<uint8_t>((blob_len >> 16) & 0xFF));
    msg_binary.push_back(static_cast<uint8_t>((blob_len >> 8) & 0xFF));
    msg_binary.push_back(static_cast<uint8_t>(blob_len & 0xFF));
    msg_binary.insert(msg_binary.end(), blob.begin(), blob.end());

    // Compute inbox_key = SHA3-256("inbox:" || user_fp)
    auto inbox_key = crypto::sha3_256_prefixed("inbox:", user_fp);

    // Simulate a Kademlia STORE arriving for this user's inbox
    server_->on_kademlia_store(inbox_key, 0x02, msg_binary);

    // Both devices should receive the NEW_MESSAGE push
    auto resp1 = device1.recv_text(3000);
    ASSERT_TRUE(resp1.has_value()) << "device 1 did not receive push notification";
    auto root1 = parse_json(*resp1);
    EXPECT_EQ(root1["type"].asString(), "NEW_MESSAGE");
    EXPECT_EQ(root1["msg_id"].asString(), to_hex(msg_id));
    EXPECT_EQ(root1["from"].asString(), to_hex(sender_fp));
    EXPECT_EQ(root1["timestamp"].asUInt64(), timestamp);

    auto resp2 = device2.recv_text(3000);
    ASSERT_TRUE(resp2.has_value()) << "device 2 did not receive push notification";
    auto root2 = parse_json(*resp2);
    EXPECT_EQ(root2["type"].asString(), "NEW_MESSAGE");
    EXPECT_EQ(root2["msg_id"].asString(), to_hex(msg_id));
    EXPECT_EQ(root2["from"].asString(), to_hex(sender_fp));
    EXPECT_EQ(root2["timestamp"].asUInt64(), timestamp);

    // Disconnect device 1 and verify device 2 still receives pushes
    device1.close();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    crypto::Hash msg_id2{};
    msg_id2.fill(0xF3);
    std::vector<uint8_t> msg_binary2;
    msg_binary2.insert(msg_binary2.end(), user_fp.begin(), user_fp.end());
    msg_binary2.insert(msg_binary2.end(), msg_id2.begin(), msg_id2.end());
    msg_binary2.insert(msg_binary2.end(), sender_fp.begin(), sender_fp.end());
    for (int i = 7; i >= 0; --i) {
        msg_binary2.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    }
    msg_binary2.push_back(static_cast<uint8_t>((blob_len >> 24) & 0xFF));
    msg_binary2.push_back(static_cast<uint8_t>((blob_len >> 16) & 0xFF));
    msg_binary2.push_back(static_cast<uint8_t>((blob_len >> 8) & 0xFF));
    msg_binary2.push_back(static_cast<uint8_t>(blob_len & 0xFF));
    msg_binary2.insert(msg_binary2.end(), blob.begin(), blob.end());

    server_->on_kademlia_store(inbox_key, 0x02, msg_binary2);

    auto resp3 = device2.recv_text(3000);
    ASSERT_TRUE(resp3.has_value()) << "device 2 did not receive second push after device 1 disconnect";
    auto root3 = parse_json(*resp3);
    EXPECT_EQ(root3["type"].asString(), "NEW_MESSAGE");
    EXPECT_EQ(root3["msg_id"].asString(), to_hex(msg_id2));

    device2.close();
}

TEST_F(WsServerTest, ResolveNameFound) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();
    auto user_fp = crypto::sha3_256(user_kp.public_key);

    // Store a name record in TABLE_NAMES
    // name record: name_len(1) || name(N) || fingerprint(32) || ...
    std::string name = "alice";
    std::vector<uint8_t> name_record;
    name_record.push_back(static_cast<uint8_t>(name.size()));
    name_record.insert(name_record.end(), name.begin(), name.end());
    name_record.insert(name_record.end(), user_fp.begin(), user_fp.end());
    // Append minimal remaining data (pow_nonce + sequence + sig_len=0)
    name_record.resize(name_record.size() + 8 + 8 + 2, 0x00);

    std::vector<uint8_t> name_bytes(name.begin(), name.end());
    auto name_key = crypto::sha3_256_prefixed("name:", name_bytes);
    storage_->put(storage::TABLE_NAMES,
        std::vector<uint8_t>(name_key.begin(), name_key.end()), name_record);

    // Connect and authenticate
    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    // Send RESOLVE_NAME
    client.send_text(R"({"type":"RESOLVE_NAME","id":100,"name":"alice"})");
    auto resp = client.recv_text(5000);
    ASSERT_TRUE(resp.has_value());
    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "RESOLVE_NAME_RESULT");
    EXPECT_EQ(root["id"].asInt(), 100);
    EXPECT_TRUE(root["found"].asBool());
    EXPECT_EQ(root["fingerprint"].asString(), to_hex(user_fp));

    client.close();
}

TEST_F(WsServerTest, ResolveNameNotFound) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();
    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    client.send_text(R"({"type":"RESOLVE_NAME","id":101,"name":"nonexistent"})");
    auto resp = client.recv_text(5000);
    ASSERT_TRUE(resp.has_value());
    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "RESOLVE_NAME_RESULT");
    EXPECT_EQ(root["id"].asInt(), 101);
    EXPECT_FALSE(root["found"].asBool());

    client.close();
}

TEST_F(WsServerTest, GetProfileFound) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();
    auto user_fp = crypto::sha3_256(user_kp.public_key);

    // Store a minimal profile in TABLE_PROFILES
    // fingerprint(32) || pubkey_len(2 BE) || pubkey || kem_pubkey_len(2 BE=0)
    // || bio_len(2 BE) || bio || avatar_len(4 BE=0) || social_count(1=0)
    // || sequence(8 BE) || sig_len(2 BE=0)
    std::vector<uint8_t> profile;
    profile.insert(profile.end(), user_fp.begin(), user_fp.end());
    // pubkey_len
    uint16_t pk_len = static_cast<uint16_t>(user_kp.public_key.size());
    profile.push_back(static_cast<uint8_t>(pk_len >> 8));
    profile.push_back(static_cast<uint8_t>(pk_len & 0xFF));
    profile.insert(profile.end(), user_kp.public_key.begin(), user_kp.public_key.end());
    // kem_pubkey_len = 0
    profile.push_back(0x00); profile.push_back(0x00);
    // bio
    std::string bio = "hello world";
    uint16_t bio_len = static_cast<uint16_t>(bio.size());
    profile.push_back(static_cast<uint8_t>(bio_len >> 8));
    profile.push_back(static_cast<uint8_t>(bio_len & 0xFF));
    profile.insert(profile.end(), bio.begin(), bio.end());
    // avatar_len = 0
    profile.push_back(0x00); profile.push_back(0x00);
    profile.push_back(0x00); profile.push_back(0x00);
    // social_count = 0
    profile.push_back(0x00);
    // sequence = 1
    for (int i = 0; i < 7; ++i) profile.push_back(0x00);
    profile.push_back(0x01);
    // sig_len = 0
    profile.push_back(0x00); profile.push_back(0x00);

    auto profile_key = crypto::sha3_256_prefixed("profile:", user_fp);
    storage_->put(storage::TABLE_PROFILES,
        std::vector<uint8_t>(profile_key.begin(), profile_key.end()), profile);

    // Connect and authenticate
    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    // Send GET_PROFILE
    std::string msg = R"({"type":"GET_PROFILE","id":102,"fingerprint":")" +
                      to_hex(user_fp) + R"("})";
    client.send_text(msg);
    auto resp = client.recv_text(5000);
    ASSERT_TRUE(resp.has_value());
    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "PROFILE_RESULT");
    EXPECT_EQ(root["id"].asInt(), 102);
    EXPECT_TRUE(root["found"].asBool());
    EXPECT_EQ(root["fingerprint"].asString(), to_hex(user_fp));
    EXPECT_EQ(root["bio"].asString(), "hello world");

    client.close();
}

TEST_F(WsServerTest, GetProfileNotFound) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();
    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    crypto::Hash fake_fp{};
    fake_fp.fill(0xDD);
    std::string msg = R"({"type":"GET_PROFILE","id":103,"fingerprint":")" +
                      to_hex(fake_fp) + R"("})";
    client.send_text(msg);
    auto resp = client.recv_text(5000);
    ASSERT_TRUE(resp.has_value());
    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "PROFILE_RESULT");
    EXPECT_EQ(root["id"].asInt(), 103);
    EXPECT_FALSE(root["found"].asBool());

    client.close();
}

TEST_F(WsServerTest, ListRequestsEmpty) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();
    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    client.send_text(R"({"type":"LIST_REQUESTS","id":104})");
    auto resp = client.recv_text(5000);
    ASSERT_TRUE(resp.has_value());
    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "LIST_REQUESTS_RESULT");
    EXPECT_EQ(root["id"].asInt(), 104);
    EXPECT_TRUE(root["requests"].isArray());
    EXPECT_EQ(root["requests"].size(), 0u);

    client.close();
}

TEST_F(WsServerTest, ListRequestsWithData) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();
    auto user_fp = crypto::sha3_256(user_kp.public_key);

    auto sender_kp = crypto::generate_keypair();
    auto sender_fp = crypto::sha3_256(sender_kp.public_key);

    // Store a contact request directly in TABLE_REQUESTS
    // key: recipient_fp(32) || sender_fp(32)
    // value: recipient_fp(32) || sender_fp(32) || pow_nonce(8) || timestamp(8) || blob_len(4) || blob
    uint64_t timestamp = 1700000000000ULL;  // some timestamp in ms
    std::vector<uint8_t> blob = {0xDE, 0xAD};
    uint32_t blob_len = static_cast<uint32_t>(blob.size());

    std::vector<uint8_t> storage_key;
    storage_key.insert(storage_key.end(), user_fp.begin(), user_fp.end());
    storage_key.insert(storage_key.end(), sender_fp.begin(), sender_fp.end());

    std::vector<uint8_t> value;
    value.insert(value.end(), user_fp.begin(), user_fp.end());
    value.insert(value.end(), sender_fp.begin(), sender_fp.end());
    // pow_nonce = 0
    for (int i = 0; i < 8; ++i) value.push_back(0x00);
    // timestamp (8 BE ms)
    for (int i = 7; i >= 0; --i)
        value.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    // blob_len (4 BE)
    value.push_back(static_cast<uint8_t>((blob_len >> 24) & 0xFF));
    value.push_back(static_cast<uint8_t>((blob_len >> 16) & 0xFF));
    value.push_back(static_cast<uint8_t>((blob_len >> 8) & 0xFF));
    value.push_back(static_cast<uint8_t>(blob_len & 0xFF));
    value.insert(value.end(), blob.begin(), blob.end());

    storage_->put(storage::TABLE_REQUESTS, storage_key, value);

    // Connect and authenticate
    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    client.send_text(R"({"type":"LIST_REQUESTS","id":105})");
    auto resp = client.recv_text(5000);
    ASSERT_TRUE(resp.has_value());
    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "LIST_REQUESTS_RESULT");
    EXPECT_EQ(root["id"].asInt(), 105);
    ASSERT_TRUE(root["requests"].isArray());
    ASSERT_EQ(root["requests"].size(), 1u);
    EXPECT_EQ(root["requests"][0]["from"].asString(), to_hex(sender_fp));
    EXPECT_EQ(root["requests"][0]["timestamp"].asUInt64(), timestamp);

    client.close();
}

TEST_F(WsServerTest, SetProfileStoresValidProfile) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();
    auto user_fp = crypto::sha3_256(user_kp.public_key);

    // Build a valid signed profile binary:
    // fingerprint(32) || pubkey_len(2 BE) || pubkey || kem_pubkey_len(2 BE=0)
    // || bio_len(2 BE) || bio || avatar_len(4 BE=0) || social_count(1=0)
    // || sequence(8 BE) || sig_len(2 BE) || signature
    std::vector<uint8_t> profile;
    profile.insert(profile.end(), user_fp.begin(), user_fp.end());
    uint16_t pk_len = static_cast<uint16_t>(user_kp.public_key.size());
    profile.push_back(static_cast<uint8_t>(pk_len >> 8));
    profile.push_back(static_cast<uint8_t>(pk_len & 0xFF));
    profile.insert(profile.end(), user_kp.public_key.begin(), user_kp.public_key.end());
    // kem_pubkey_len = 0
    profile.push_back(0x00); profile.push_back(0x00);
    // bio = "test bio"
    std::string bio = "test bio";
    uint16_t bio_len = static_cast<uint16_t>(bio.size());
    profile.push_back(static_cast<uint8_t>(bio_len >> 8));
    profile.push_back(static_cast<uint8_t>(bio_len & 0xFF));
    profile.insert(profile.end(), bio.begin(), bio.end());
    // avatar_len = 0
    for (int i = 0; i < 4; ++i) profile.push_back(0x00);
    // social_count = 0
    profile.push_back(0x00);
    // sequence = 1
    for (int i = 0; i < 7; ++i) profile.push_back(0x00);
    profile.push_back(0x01);

    // Sign everything up to this point
    auto signature = crypto::sign(profile, user_kp.secret_key);
    uint16_t sig_len = static_cast<uint16_t>(signature.size());
    profile.push_back(static_cast<uint8_t>(sig_len >> 8));
    profile.push_back(static_cast<uint8_t>(sig_len & 0xFF));
    profile.insert(profile.end(), signature.begin(), signature.end());

    // Connect and authenticate
    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    // Send SET_PROFILE
    std::string msg = R"({"type":"SET_PROFILE","id":200,"profile":")" +
                      to_base64(profile) + R"("})";
    client.send_text(msg);
    auto resp = client.recv_text(5000);
    ASSERT_TRUE(resp.has_value()) << "no response to SET_PROFILE";
    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "SET_PROFILE_ACK");
    EXPECT_EQ(root["id"].asInt(), 200);

    // Verify stored
    auto profile_key = crypto::sha3_256_prefixed("profile:", user_fp);
    auto stored = storage_->get(storage::TABLE_PROFILES,
        std::vector<uint8_t>(profile_key.begin(), profile_key.end()));
    EXPECT_TRUE(stored.has_value()) << "profile should be stored";

    client.close();
}

TEST_F(WsServerTest, SetProfileRejectsMismatchedFingerprint) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();
    auto other_kp = crypto::generate_keypair();
    auto other_fp = crypto::sha3_256(other_kp.public_key);

    // Build a profile with other_fp (not the authenticated user's)
    std::vector<uint8_t> profile;
    profile.insert(profile.end(), other_fp.begin(), other_fp.end());
    profile.resize(53, 0x00);  // minimal padding

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    std::string msg = R"({"type":"SET_PROFILE","id":201,"profile":")" +
                      to_base64(profile) + R"("})";
    client.send_text(msg);
    auto resp = client.recv_text(5000);
    ASSERT_TRUE(resp.has_value());
    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "ERROR");
    EXPECT_EQ(root["code"].asInt(), 403);

    client.close();
}

TEST_F(WsServerTest, RegisterNameRejectsMismatchedFingerprint) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();
    auto other_kp = crypto::generate_keypair();
    auto other_fp = crypto::sha3_256(other_kp.public_key);

    // Build a name record with other_fp
    std::string name = "alice";
    std::vector<uint8_t> record;
    record.push_back(static_cast<uint8_t>(name.size()));
    record.insert(record.end(), name.begin(), name.end());
    record.insert(record.end(), other_fp.begin(), other_fp.end());
    record.resize(record.size() + 20, 0x00);

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    std::string msg = R"({"type":"REGISTER_NAME","id":202,"name_record":")" +
                      to_base64(record) + R"("})";
    client.send_text(msg);
    auto resp = client.recv_text(5000);
    ASSERT_TRUE(resp.has_value());
    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "ERROR");
    EXPECT_EQ(root["code"].asInt(), 403);

    client.close();
}

// ---------- GROUP_CREATE / GROUP_INFO tests ----------

TEST_F(WsServerTest, GroupCreateAndInfo) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();
    auto user_fp = crypto::sha3_256(user_kp.public_key);

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    // Build a random group_id
    crypto::Hash group_id{};
    for (auto& b : group_id) b = static_cast<uint8_t>(rand() & 0xFF);

    // Build GROUP_META with the client as owner
    auto meta = build_group_meta(group_id, 1, {{user_fp, 0x02}}, user_kp);

    // GROUP_CREATE
    Json::Value create_cmd;
    create_cmd["type"] = "GROUP_CREATE";
    create_cmd["id"] = 10;
    create_cmd["group_meta"] = to_hex(meta);

    auto resp_str = send_cmd(client, create_cmd, 5000);
    ASSERT_FALSE(resp_str.empty());
    auto resp = parse_json(resp_str);
    EXPECT_EQ(resp["id"].asInt(), 10);
    EXPECT_TRUE(resp["ok"].asBool()) << "GROUP_CREATE failed: " << resp_str;
    EXPECT_EQ(resp["group_id"].asString(), to_hex(group_id));

    // GROUP_INFO — same client should be a member
    Json::Value info_cmd;
    info_cmd["type"] = "GROUP_INFO";
    info_cmd["id"] = 11;
    info_cmd["group_id"] = to_hex(group_id);

    auto info_str = send_cmd(client, info_cmd, 5000);
    ASSERT_FALSE(info_str.empty());
    auto info = parse_json(info_str);
    EXPECT_EQ(info["id"].asInt(), 11);
    EXPECT_TRUE(info["ok"].asBool()) << "GROUP_INFO failed: " << info_str;
    EXPECT_FALSE(info["group_meta"].asString().empty());
    // Returned meta should match what we stored
    EXPECT_EQ(info["group_meta"].asString(), to_hex(meta));

    client.close();
}

TEST_F(WsServerTest, GroupInfoNotMember) {
    start_ws_server();

    auto owner_kp = crypto::generate_keypair();
    auto owner_fp = crypto::sha3_256(owner_kp.public_key);

    auto other_kp = crypto::generate_keypair();

    TestWsClient owner_client;
    ASSERT_TRUE(owner_client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(owner_client, owner_kp));

    // Build group with only the owner
    crypto::Hash group_id{};
    for (auto& b : group_id) b = static_cast<uint8_t>(rand() & 0xFF);

    auto meta = build_group_meta(group_id, 1, {{owner_fp, 0x02}}, owner_kp);

    Json::Value create_cmd;
    create_cmd["type"] = "GROUP_CREATE";
    create_cmd["id"] = 20;
    create_cmd["group_meta"] = to_hex(meta);

    auto create_str = send_cmd(owner_client, create_cmd, 5000);
    ASSERT_FALSE(create_str.empty());
    auto create_resp = parse_json(create_str);
    EXPECT_TRUE(create_resp["ok"].asBool()) << "GROUP_CREATE failed: " << create_str;

    owner_client.close();

    // Connect as non-member and try GROUP_INFO
    TestWsClient other_client;
    ASSERT_TRUE(other_client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(other_client, other_kp));

    Json::Value info_cmd;
    info_cmd["type"] = "GROUP_INFO";
    info_cmd["id"] = 21;
    info_cmd["group_id"] = to_hex(group_id);

    auto info_str = send_cmd(other_client, info_cmd, 5000);
    ASSERT_FALSE(info_str.empty());
    auto info = parse_json(info_str);
    EXPECT_EQ(info["type"].asString(), "ERROR");
    EXPECT_EQ(info["code"].asInt(), 403);

    other_client.close();
}

TEST_F(WsServerTest, GroupCreateDuplicate) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();
    auto user_fp = crypto::sha3_256(user_kp.public_key);

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    crypto::Hash group_id{};
    for (auto& b : group_id) b = static_cast<uint8_t>(rand() & 0xFF);

    auto meta = build_group_meta(group_id, 1, {{user_fp, 0x02}}, user_kp);

    // First create should succeed
    Json::Value cmd;
    cmd["type"] = "GROUP_CREATE";
    cmd["id"] = 30;
    cmd["group_meta"] = to_hex(meta);

    auto resp1_str = send_cmd(client, cmd, 5000);
    auto resp1 = parse_json(resp1_str);
    EXPECT_TRUE(resp1["ok"].asBool());

    // Second create with same group_id should get 409
    cmd["id"] = 31;
    auto resp2_str = send_cmd(client, cmd, 5000);
    auto resp2 = parse_json(resp2_str);
    EXPECT_EQ(resp2["type"].asString(), "ERROR");
    EXPECT_EQ(resp2["code"].asInt(), 409);

    client.close();
}

TEST_F(WsServerTest, GroupCreateNotOwner) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();
    auto user_fp = crypto::sha3_256(user_kp.public_key);

    auto other_kp = crypto::generate_keypair();
    auto other_fp = crypto::sha3_256(other_kp.public_key);

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    crypto::Hash group_id{};
    for (auto& b : group_id) b = static_cast<uint8_t>(rand() & 0xFF);

    // Build meta where authenticated user is only a member, not owner
    auto meta = build_group_meta(group_id, 1,
                                 {{other_fp, 0x02}, {user_fp, 0x00}}, other_kp);

    Json::Value cmd;
    cmd["type"] = "GROUP_CREATE";
    cmd["id"] = 40;
    cmd["group_meta"] = to_hex(meta);

    auto resp_str = send_cmd(client, cmd, 5000);
    auto resp = parse_json(resp_str);
    EXPECT_EQ(resp["type"].asString(), "ERROR");
    EXPECT_EQ(resp["code"].asInt(), 403);

    client.close();
}

// ---------- GROUP_SEND / GROUP_LIST tests ----------

TEST_F(WsServerTest, GroupSendAndList) {
    start_ws_server();

    auto owner_kp = crypto::generate_keypair();
    auto owner_fp = crypto::sha3_256(owner_kp.public_key);

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, owner_kp));

    // Create a group
    crypto::Hash group_id{};
    for (auto& b : group_id) b = static_cast<uint8_t>(rand() & 0xFF);

    auto meta = build_group_meta(group_id, 1, {{owner_fp, 0x02}}, owner_kp);

    Json::Value create_cmd;
    create_cmd["type"] = "GROUP_CREATE";
    create_cmd["id"] = 100;
    create_cmd["group_meta"] = to_hex(meta);

    auto create_str = send_cmd(client, create_cmd, 5000);
    ASSERT_FALSE(create_str.empty());
    auto create_resp = parse_json(create_str);
    ASSERT_TRUE(create_resp["ok"].asBool()) << "GROUP_CREATE failed: " << create_str;

    // Generate a random msg_id and a small blob
    crypto::Hash msg_id{};
    for (auto& b : msg_id) b = static_cast<uint8_t>(rand() & 0xFF);
    std::vector<uint8_t> blob(16);
    for (auto& b : blob) b = static_cast<uint8_t>(rand() & 0xFF);

    // GROUP_SEND
    Json::Value send_cmd_val;
    send_cmd_val["type"] = "GROUP_SEND";
    send_cmd_val["id"] = 101;
    send_cmd_val["group_id"] = to_hex(group_id);
    send_cmd_val["msg_id"] = to_hex(msg_id);
    send_cmd_val["gek_version"] = 1;
    send_cmd_val["blob"] = to_hex(blob);

    auto send_str = send_cmd(client, send_cmd_val, 5000);
    ASSERT_FALSE(send_str.empty());
    auto send_resp = parse_json(send_str);
    EXPECT_EQ(send_resp["id"].asInt(), 101);
    EXPECT_TRUE(send_resp["ok"].asBool()) << "GROUP_SEND failed: " << send_str;
    EXPECT_EQ(send_resp["msg_id"].asString(), to_hex(msg_id));

    // GROUP_LIST
    Json::Value list_cmd;
    list_cmd["type"] = "GROUP_LIST";
    list_cmd["id"] = 102;
    list_cmd["group_id"] = to_hex(group_id);

    auto list_str = send_cmd(client, list_cmd, 5000);
    ASSERT_FALSE(list_str.empty());
    auto list_resp = parse_json(list_str);
    EXPECT_EQ(list_resp["id"].asInt(), 102);
    EXPECT_TRUE(list_resp["ok"].asBool()) << "GROUP_LIST failed: " << list_str;

    auto& messages = list_resp["messages"];
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0]["msg_id"].asString(), to_hex(msg_id));
    EXPECT_EQ(messages[0]["sender"].asString(), to_hex(owner_fp));
    EXPECT_EQ(messages[0]["size"].asUInt(), blob.size());
    EXPECT_EQ(messages[0]["gek_version"].asUInt(), 1u);
    EXPECT_EQ(messages[0]["blob"].asString(), to_hex(blob));

    client.close();
}

TEST_F(WsServerTest, GroupSendNotMember) {
    start_ws_server();

    auto owner_kp = crypto::generate_keypair();
    auto owner_fp = crypto::sha3_256(owner_kp.public_key);

    // Create group as owner
    TestWsClient owner_client;
    ASSERT_TRUE(owner_client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(owner_client, owner_kp));

    crypto::Hash group_id{};
    for (auto& b : group_id) b = static_cast<uint8_t>(rand() & 0xFF);

    auto meta = build_group_meta(group_id, 1, {{owner_fp, 0x02}}, owner_kp);

    Json::Value create_cmd;
    create_cmd["type"] = "GROUP_CREATE";
    create_cmd["id"] = 200;
    create_cmd["group_meta"] = to_hex(meta);

    auto create_str = send_cmd(owner_client, create_cmd, 5000);
    ASSERT_FALSE(create_str.empty());
    auto create_resp = parse_json(create_str);
    ASSERT_TRUE(create_resp["ok"].asBool()) << "GROUP_CREATE failed: " << create_str;
    owner_client.close();

    // Authenticate as a non-member
    auto other_kp = crypto::generate_keypair();
    TestWsClient other_client;
    ASSERT_TRUE(other_client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(other_client, other_kp));

    // Try GROUP_SEND as non-member
    crypto::Hash msg_id{};
    for (auto& b : msg_id) b = static_cast<uint8_t>(rand() & 0xFF);
    std::vector<uint8_t> blob(16, 0xAB);

    Json::Value send_cmd_val;
    send_cmd_val["type"] = "GROUP_SEND";
    send_cmd_val["id"] = 201;
    send_cmd_val["group_id"] = to_hex(group_id);
    send_cmd_val["msg_id"] = to_hex(msg_id);
    send_cmd_val["gek_version"] = 1;
    send_cmd_val["blob"] = to_hex(blob);

    auto send_str = send_cmd(other_client, send_cmd_val, 5000);
    ASSERT_FALSE(send_str.empty());
    auto send_resp = parse_json(send_str);
    EXPECT_EQ(send_resp["type"].asString(), "ERROR");
    EXPECT_EQ(send_resp["code"].asInt(), 403);

    other_client.close();
}

TEST_F(WsServerTest, GroupListPagination) {
    start_ws_server();

    auto owner_kp = crypto::generate_keypair();
    auto owner_fp = crypto::sha3_256(owner_kp.public_key);

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, owner_kp));

    // Create a group
    crypto::Hash group_id{};
    for (auto& b : group_id) b = static_cast<uint8_t>(rand() & 0xFF);

    auto meta = build_group_meta(group_id, 1, {{owner_fp, 0x02}}, owner_kp);

    Json::Value create_cmd;
    create_cmd["type"] = "GROUP_CREATE";
    create_cmd["id"] = 300;
    create_cmd["group_meta"] = to_hex(meta);

    auto create_str = send_cmd(client, create_cmd, 5000);
    ASSERT_FALSE(create_str.empty());
    auto create_resp = parse_json(create_str);
    ASSERT_TRUE(create_resp["ok"].asBool()) << "GROUP_CREATE failed: " << create_str;

    // Send 5 messages with deterministic msg_ids (sorted lexicographically)
    std::vector<crypto::Hash> msg_ids(5);
    for (int i = 0; i < 5; ++i) {
        // Use deterministic msg_ids that sort in known order:
        // First byte determines sort order, rest are zeros
        msg_ids[i] = {};
        msg_ids[i][0] = static_cast<uint8_t>(i + 1);  // 0x01..0x05
    }

    for (int i = 0; i < 5; ++i) {
        std::vector<uint8_t> blob(8, static_cast<uint8_t>(i + 0x10));

        Json::Value send_val;
        send_val["type"] = "GROUP_SEND";
        send_val["id"] = 310 + i;
        send_val["group_id"] = to_hex(group_id);
        send_val["msg_id"] = to_hex(msg_ids[i]);
        send_val["gek_version"] = 1;
        send_val["blob"] = to_hex(blob);

        auto send_str = send_cmd(client, send_val, 5000);
        ASSERT_FALSE(send_str.empty());
        auto send_resp = parse_json(send_str);
        ASSERT_TRUE(send_resp["ok"].asBool()) << "GROUP_SEND #" << i << " failed: " << send_str;
    }

    // GROUP_LIST with limit=2 -> expect 2 messages
    Json::Value list_cmd1;
    list_cmd1["type"] = "GROUP_LIST";
    list_cmd1["id"] = 320;
    list_cmd1["group_id"] = to_hex(group_id);
    list_cmd1["limit"] = 2;

    auto list1_str = send_cmd(client, list_cmd1, 5000);
    ASSERT_FALSE(list1_str.empty());
    auto list1 = parse_json(list1_str);
    EXPECT_TRUE(list1["ok"].asBool()) << "GROUP_LIST page1 failed: " << list1_str;
    ASSERT_EQ(list1["messages"].size(), 2u);

    // Get the last msg_id from page 1
    std::string last_msg_id = list1["messages"][1u]["msg_id"].asString();

    // GROUP_LIST with after=last_msg_id, limit=2 -> expect 2 more messages
    Json::Value list_cmd2;
    list_cmd2["type"] = "GROUP_LIST";
    list_cmd2["id"] = 321;
    list_cmd2["group_id"] = to_hex(group_id);
    list_cmd2["after"] = last_msg_id;
    list_cmd2["limit"] = 2;

    auto list2_str = send_cmd(client, list_cmd2, 5000);
    ASSERT_FALSE(list2_str.empty());
    auto list2 = parse_json(list2_str);
    EXPECT_TRUE(list2["ok"].asBool()) << "GROUP_LIST page2 failed: " << list2_str;
    ASSERT_EQ(list2["messages"].size(), 2u);

    // Verify page 2 msg_ids are different from page 1
    EXPECT_NE(list2["messages"][0u]["msg_id"].asString(), list1["messages"][0u]["msg_id"].asString());
    EXPECT_NE(list2["messages"][0u]["msg_id"].asString(), list1["messages"][1u]["msg_id"].asString());

    client.close();
}

TEST_F(WsServerTest, GroupGetMessage) {
    start_ws_server();

    auto owner_kp = crypto::generate_keypair();
    auto owner_fp = crypto::sha3_256(owner_kp.public_key);

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, owner_kp));

    // Create a group
    crypto::Hash group_id{};
    for (auto& b : group_id) b = static_cast<uint8_t>(rand() & 0xFF);

    auto meta = build_group_meta(group_id, 1, {{owner_fp, 0x02}}, owner_kp);

    Json::Value create_cmd;
    create_cmd["type"] = "GROUP_CREATE";
    create_cmd["id"] = 400;
    create_cmd["group_meta"] = to_hex(meta);

    auto create_str = send_cmd(client, create_cmd, 5000);
    ASSERT_FALSE(create_str.empty());
    auto create_resp = parse_json(create_str);
    ASSERT_TRUE(create_resp["ok"].asBool()) << "GROUP_CREATE failed: " << create_str;

    // Generate a random msg_id and a small blob
    crypto::Hash msg_id{};
    for (auto& b : msg_id) b = static_cast<uint8_t>(rand() & 0xFF);
    std::vector<uint8_t> blob(16);
    for (auto& b : blob) b = static_cast<uint8_t>(rand() & 0xFF);

    // GROUP_SEND
    Json::Value send_val;
    send_val["type"] = "GROUP_SEND";
    send_val["id"] = 401;
    send_val["group_id"] = to_hex(group_id);
    send_val["msg_id"] = to_hex(msg_id);
    send_val["gek_version"] = 1;
    send_val["blob"] = to_hex(blob);

    auto send_str = send_cmd(client, send_val, 5000);
    ASSERT_FALSE(send_str.empty());
    auto send_resp = parse_json(send_str);
    ASSERT_TRUE(send_resp["ok"].asBool()) << "GROUP_SEND failed: " << send_str;

    // GROUP_GET
    Json::Value get_cmd;
    get_cmd["type"] = "GROUP_GET";
    get_cmd["id"] = 402;
    get_cmd["group_id"] = to_hex(group_id);
    get_cmd["msg_id"] = to_hex(msg_id);

    auto get_str = send_cmd(client, get_cmd, 5000);
    ASSERT_FALSE(get_str.empty());
    auto get_resp = parse_json(get_str);
    EXPECT_EQ(get_resp["id"].asInt(), 402);
    EXPECT_TRUE(get_resp["ok"].asBool()) << "GROUP_GET failed: " << get_str;
    EXPECT_EQ(get_resp["blob"].asString(), to_hex(blob));

    client.close();
}

TEST_F(WsServerTest, GroupDeleteOwnMessage) {
    start_ws_server();

    auto owner_kp = crypto::generate_keypair();
    auto owner_fp = crypto::sha3_256(owner_kp.public_key);

    auto member_kp = crypto::generate_keypair();
    auto member_fp = crypto::sha3_256(member_kp.public_key);

    // Create group with owner + member
    crypto::Hash group_id{};
    for (auto& b : group_id) b = static_cast<uint8_t>(rand() & 0xFF);

    auto meta = build_group_meta(group_id, 1,
        {{owner_fp, 0x02}, {member_fp, 0x00}}, owner_kp);

    // Owner creates the group
    TestWsClient owner_client;
    ASSERT_TRUE(owner_client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(owner_client, owner_kp));

    Json::Value create_cmd;
    create_cmd["type"] = "GROUP_CREATE";
    create_cmd["id"] = 500;
    create_cmd["group_meta"] = to_hex(meta);

    auto create_str = send_cmd(owner_client, create_cmd, 5000);
    ASSERT_FALSE(create_str.empty());
    auto create_resp = parse_json(create_str);
    ASSERT_TRUE(create_resp["ok"].asBool()) << "GROUP_CREATE failed: " << create_str;
    owner_client.close();

    // Authenticate as member and send a message
    TestWsClient member_client;
    ASSERT_TRUE(member_client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(member_client, member_kp));

    crypto::Hash msg_id{};
    for (auto& b : msg_id) b = static_cast<uint8_t>(rand() & 0xFF);
    std::vector<uint8_t> blob(16, 0xAB);

    Json::Value send_val;
    send_val["type"] = "GROUP_SEND";
    send_val["id"] = 501;
    send_val["group_id"] = to_hex(group_id);
    send_val["msg_id"] = to_hex(msg_id);
    send_val["gek_version"] = 1;
    send_val["blob"] = to_hex(blob);

    auto send_str = send_cmd(member_client, send_val, 5000);
    ASSERT_FALSE(send_str.empty());
    auto send_resp = parse_json(send_str);
    ASSERT_TRUE(send_resp["ok"].asBool()) << "GROUP_SEND failed: " << send_str;

    // Member deletes their own message
    Json::Value del_cmd;
    del_cmd["type"] = "GROUP_DELETE";
    del_cmd["id"] = 502;
    del_cmd["group_id"] = to_hex(group_id);
    del_cmd["msg_id"] = to_hex(msg_id);

    auto del_str = send_cmd(member_client, del_cmd, 5000);
    ASSERT_FALSE(del_str.empty());
    auto del_resp = parse_json(del_str);
    EXPECT_EQ(del_resp["id"].asInt(), 502);
    EXPECT_TRUE(del_resp["ok"].asBool()) << "GROUP_DELETE failed: " << del_str;

    // GROUP_GET for deleted message should return 404
    Json::Value get_cmd;
    get_cmd["type"] = "GROUP_GET";
    get_cmd["id"] = 503;
    get_cmd["group_id"] = to_hex(group_id);
    get_cmd["msg_id"] = to_hex(msg_id);

    auto get_str = send_cmd(member_client, get_cmd, 5000);
    ASSERT_FALSE(get_str.empty());
    auto get_resp = parse_json(get_str);
    EXPECT_EQ(get_resp["type"].asString(), "ERROR");
    EXPECT_EQ(get_resp["code"].asInt(), 404);

    member_client.close();
}

TEST_F(WsServerTest, GroupDeleteByAdmin) {
    start_ws_server();

    auto owner_kp = crypto::generate_keypair();
    auto owner_fp = crypto::sha3_256(owner_kp.public_key);

    auto admin_kp = crypto::generate_keypair();
    auto admin_fp = crypto::sha3_256(admin_kp.public_key);

    auto member_kp = crypto::generate_keypair();
    auto member_fp = crypto::sha3_256(member_kp.public_key);

    // Create group with owner + admin + member
    crypto::Hash group_id{};
    for (auto& b : group_id) b = static_cast<uint8_t>(rand() & 0xFF);

    auto meta = build_group_meta(group_id, 1,
        {{owner_fp, 0x02}, {admin_fp, 0x01}, {member_fp, 0x00}}, owner_kp);

    // Owner creates the group
    TestWsClient owner_client;
    ASSERT_TRUE(owner_client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(owner_client, owner_kp));

    Json::Value create_cmd;
    create_cmd["type"] = "GROUP_CREATE";
    create_cmd["id"] = 600;
    create_cmd["group_meta"] = to_hex(meta);

    auto create_str = send_cmd(owner_client, create_cmd, 5000);
    ASSERT_FALSE(create_str.empty());
    auto create_resp = parse_json(create_str);
    ASSERT_TRUE(create_resp["ok"].asBool()) << "GROUP_CREATE failed: " << create_str;
    owner_client.close();

    // Member sends a message
    TestWsClient member_client;
    ASSERT_TRUE(member_client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(member_client, member_kp));

    crypto::Hash msg_id{};
    for (auto& b : msg_id) b = static_cast<uint8_t>(rand() & 0xFF);
    std::vector<uint8_t> blob(16, 0xCD);

    Json::Value send_val;
    send_val["type"] = "GROUP_SEND";
    send_val["id"] = 601;
    send_val["group_id"] = to_hex(group_id);
    send_val["msg_id"] = to_hex(msg_id);
    send_val["gek_version"] = 1;
    send_val["blob"] = to_hex(blob);

    auto send_str = send_cmd(member_client, send_val, 5000);
    ASSERT_FALSE(send_str.empty());
    auto send_resp = parse_json(send_str);
    ASSERT_TRUE(send_resp["ok"].asBool()) << "GROUP_SEND failed: " << send_str;
    member_client.close();

    // Admin deletes the member's message
    TestWsClient admin_client;
    ASSERT_TRUE(admin_client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(admin_client, admin_kp));

    Json::Value del_cmd;
    del_cmd["type"] = "GROUP_DELETE";
    del_cmd["id"] = 602;
    del_cmd["group_id"] = to_hex(group_id);
    del_cmd["msg_id"] = to_hex(msg_id);

    auto del_str = send_cmd(admin_client, del_cmd, 5000);
    ASSERT_FALSE(del_str.empty());
    auto del_resp = parse_json(del_str);
    EXPECT_EQ(del_resp["id"].asInt(), 602);
    EXPECT_TRUE(del_resp["ok"].asBool()) << "GROUP_DELETE by admin failed: " << del_str;

    admin_client.close();
}

TEST_F(WsServerTest, GroupDeleteForbidden) {
    start_ws_server();

    auto owner_kp = crypto::generate_keypair();
    auto owner_fp = crypto::sha3_256(owner_kp.public_key);

    auto member1_kp = crypto::generate_keypair();
    auto member1_fp = crypto::sha3_256(member1_kp.public_key);

    auto member2_kp = crypto::generate_keypair();
    auto member2_fp = crypto::sha3_256(member2_kp.public_key);

    // Create group with owner + member1 + member2 (no admin)
    crypto::Hash group_id{};
    for (auto& b : group_id) b = static_cast<uint8_t>(rand() & 0xFF);

    auto meta = build_group_meta(group_id, 1,
        {{owner_fp, 0x02}, {member1_fp, 0x00}, {member2_fp, 0x00}}, owner_kp);

    // Owner creates the group
    TestWsClient owner_client;
    ASSERT_TRUE(owner_client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(owner_client, owner_kp));

    Json::Value create_cmd;
    create_cmd["type"] = "GROUP_CREATE";
    create_cmd["id"] = 700;
    create_cmd["group_meta"] = to_hex(meta);

    auto create_str = send_cmd(owner_client, create_cmd, 5000);
    ASSERT_FALSE(create_str.empty());
    auto create_resp = parse_json(create_str);
    ASSERT_TRUE(create_resp["ok"].asBool()) << "GROUP_CREATE failed: " << create_str;
    owner_client.close();

    // Member1 sends a message
    TestWsClient member1_client;
    ASSERT_TRUE(member1_client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(member1_client, member1_kp));

    crypto::Hash msg_id{};
    for (auto& b : msg_id) b = static_cast<uint8_t>(rand() & 0xFF);
    std::vector<uint8_t> blob(16, 0xEF);

    Json::Value send_val;
    send_val["type"] = "GROUP_SEND";
    send_val["id"] = 701;
    send_val["group_id"] = to_hex(group_id);
    send_val["msg_id"] = to_hex(msg_id);
    send_val["gek_version"] = 1;
    send_val["blob"] = to_hex(blob);

    auto send_str = send_cmd(member1_client, send_val, 5000);
    ASSERT_FALSE(send_str.empty());
    auto send_resp = parse_json(send_str);
    ASSERT_TRUE(send_resp["ok"].asBool()) << "GROUP_SEND failed: " << send_str;
    member1_client.close();

    // Member2 tries to delete member1's message — should be forbidden
    TestWsClient member2_client;
    ASSERT_TRUE(member2_client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(member2_client, member2_kp));

    Json::Value del_cmd;
    del_cmd["type"] = "GROUP_DELETE";
    del_cmd["id"] = 702;
    del_cmd["group_id"] = to_hex(group_id);
    del_cmd["msg_id"] = to_hex(msg_id);

    auto del_str = send_cmd(member2_client, del_cmd, 5000);
    ASSERT_FALSE(del_str.empty());
    auto del_resp = parse_json(del_str);
    EXPECT_EQ(del_resp["type"].asString(), "ERROR");
    EXPECT_EQ(del_resp["code"].asInt(), 403);

    member2_client.close();
}

TEST_F(WsServerTest, GroupUpdateAddMember) {
    start_ws_server();

    auto owner_kp = crypto::generate_keypair();
    auto owner_fp = crypto::sha3_256(owner_kp.public_key);

    auto member_kp = crypto::generate_keypair();
    auto member_fp = crypto::sha3_256(member_kp.public_key);

    // Generate random group_id
    crypto::Hash group_id{};
    for (auto& b : group_id) b = static_cast<uint8_t>(rand() & 0xFF);

    // Create group with version=1, owner only
    auto meta_v1 = build_group_meta(group_id, 1, {{owner_fp, 0x02}}, owner_kp);

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, owner_kp));

    Json::Value create_cmd;
    create_cmd["type"] = "GROUP_CREATE";
    create_cmd["id"] = 800;
    create_cmd["group_meta"] = to_hex(meta_v1);

    auto create_str = send_cmd(client, create_cmd, 5000);
    ASSERT_FALSE(create_str.empty());
    auto create_resp = parse_json(create_str);
    ASSERT_TRUE(create_resp["ok"].asBool()) << "GROUP_CREATE failed: " << create_str;

    // GROUP_UPDATE: version=2, owner + member
    auto meta_v2 = build_group_meta(group_id, 2,
        {{owner_fp, 0x02}, {member_fp, 0x00}}, owner_kp);

    Json::Value update_cmd;
    update_cmd["type"] = "GROUP_UPDATE";
    update_cmd["id"] = 801;
    update_cmd["group_meta"] = to_hex(meta_v2);

    auto update_str = send_cmd(client, update_cmd, 5000);
    ASSERT_FALSE(update_str.empty());
    auto update_resp = parse_json(update_str);
    EXPECT_TRUE(update_resp["ok"].asBool()) << "GROUP_UPDATE failed: " << update_str;

    // GROUP_INFO: verify updated meta has 2 members
    Json::Value info_cmd;
    info_cmd["type"] = "GROUP_INFO";
    info_cmd["id"] = 802;
    info_cmd["group_id"] = to_hex(group_id);

    auto info_str = send_cmd(client, info_cmd, 5000);
    ASSERT_FALSE(info_str.empty());
    auto info_resp = parse_json(info_str);
    EXPECT_TRUE(info_resp["ok"].asBool()) << "GROUP_INFO failed: " << info_str;

    // Parse the returned group_meta to verify 2 members
    auto returned_hex = info_resp["group_meta"].asString();
    auto returned_bytes = from_hex(returned_hex);
    ASSERT_GE(returned_bytes.size(), 70u);
    // member_count at offset 68-69 (BE)
    uint16_t member_count = (static_cast<uint16_t>(returned_bytes[68]) << 8) |
                             static_cast<uint16_t>(returned_bytes[69]);
    EXPECT_EQ(member_count, 2u);

    client.close();
}

TEST_F(WsServerTest, GroupUpdateVersionMustIncrease) {
    start_ws_server();

    auto owner_kp = crypto::generate_keypair();
    auto owner_fp = crypto::sha3_256(owner_kp.public_key);

    crypto::Hash group_id{};
    for (auto& b : group_id) b = static_cast<uint8_t>(rand() & 0xFF);

    // Create group with version=1
    auto meta_v1 = build_group_meta(group_id, 1, {{owner_fp, 0x02}}, owner_kp);

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, owner_kp));

    Json::Value create_cmd;
    create_cmd["type"] = "GROUP_CREATE";
    create_cmd["id"] = 810;
    create_cmd["group_meta"] = to_hex(meta_v1);

    auto create_str = send_cmd(client, create_cmd, 5000);
    ASSERT_FALSE(create_str.empty());
    auto create_resp = parse_json(create_str);
    ASSERT_TRUE(create_resp["ok"].asBool()) << "GROUP_CREATE failed: " << create_str;

    // GROUP_UPDATE with same version=1 — should fail 409
    auto meta_same = build_group_meta(group_id, 1, {{owner_fp, 0x02}}, owner_kp);

    Json::Value update_cmd;
    update_cmd["type"] = "GROUP_UPDATE";
    update_cmd["id"] = 811;
    update_cmd["group_meta"] = to_hex(meta_same);

    auto update_str = send_cmd(client, update_cmd, 5000);
    ASSERT_FALSE(update_str.empty());
    auto update_resp = parse_json(update_str);
    EXPECT_EQ(update_resp["type"].asString(), "ERROR");
    EXPECT_EQ(update_resp["code"].asInt(), 409);

    client.close();
}

TEST_F(WsServerTest, GroupUpdateNonMemberRejected) {
    start_ws_server();

    auto owner_kp = crypto::generate_keypair();
    auto owner_fp = crypto::sha3_256(owner_kp.public_key);

    auto outsider_kp = crypto::generate_keypair();
    auto outsider_fp = crypto::sha3_256(outsider_kp.public_key);

    crypto::Hash group_id{};
    for (auto& b : group_id) b = static_cast<uint8_t>(rand() & 0xFF);

    // Create group as owner
    auto meta_v1 = build_group_meta(group_id, 1, {{owner_fp, 0x02}}, owner_kp);

    TestWsClient owner_client;
    ASSERT_TRUE(owner_client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(owner_client, owner_kp));

    Json::Value create_cmd;
    create_cmd["type"] = "GROUP_CREATE";
    create_cmd["id"] = 820;
    create_cmd["group_meta"] = to_hex(meta_v1);

    auto create_str = send_cmd(owner_client, create_cmd, 5000);
    ASSERT_FALSE(create_str.empty());
    auto create_resp = parse_json(create_str);
    ASSERT_TRUE(create_resp["ok"].asBool()) << "GROUP_CREATE failed: " << create_str;
    owner_client.close();

    // Outsider tries to update — should fail 403
    TestWsClient outsider_client;
    ASSERT_TRUE(outsider_client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(outsider_client, outsider_kp));

    auto meta_v2 = build_group_meta(group_id, 2,
        {{owner_fp, 0x02}, {outsider_fp, 0x00}}, owner_kp);

    Json::Value update_cmd;
    update_cmd["type"] = "GROUP_UPDATE";
    update_cmd["id"] = 821;
    update_cmd["group_meta"] = to_hex(meta_v2);

    auto update_str = send_cmd(outsider_client, update_cmd, 5000);
    ASSERT_FALSE(update_str.empty());
    auto update_resp = parse_json(update_str);
    EXPECT_EQ(update_resp["type"].asString(), "ERROR");
    EXPECT_EQ(update_resp["code"].asInt(), 403);

    outsider_client.close();
}

TEST_F(WsServerTest, GroupUpdateAdminCanAddMember) {
    start_ws_server();

    auto owner_kp = crypto::generate_keypair();
    auto owner_fp = crypto::sha3_256(owner_kp.public_key);

    auto admin_kp = crypto::generate_keypair();
    auto admin_fp = crypto::sha3_256(admin_kp.public_key);

    auto member_kp = crypto::generate_keypair();
    auto member_fp = crypto::sha3_256(member_kp.public_key);

    crypto::Hash group_id{};
    for (auto& b : group_id) b = static_cast<uint8_t>(rand() & 0xFF);

    // Create group with owner + admin
    auto meta_v1 = build_group_meta(group_id, 1,
        {{owner_fp, 0x02}, {admin_fp, 0x01}}, owner_kp);

    TestWsClient owner_client;
    ASSERT_TRUE(owner_client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(owner_client, owner_kp));

    Json::Value create_cmd;
    create_cmd["type"] = "GROUP_CREATE";
    create_cmd["id"] = 830;
    create_cmd["group_meta"] = to_hex(meta_v1);

    auto create_str = send_cmd(owner_client, create_cmd, 5000);
    ASSERT_FALSE(create_str.empty());
    auto create_resp = parse_json(create_str);
    ASSERT_TRUE(create_resp["ok"].asBool()) << "GROUP_CREATE failed: " << create_str;
    owner_client.close();

    // Admin adds a new regular member
    TestWsClient admin_client;
    ASSERT_TRUE(admin_client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(admin_client, admin_kp));

    auto meta_v2 = build_group_meta(group_id, 2,
        {{owner_fp, 0x02}, {admin_fp, 0x01}, {member_fp, 0x00}}, owner_kp);

    Json::Value update_cmd;
    update_cmd["type"] = "GROUP_UPDATE";
    update_cmd["id"] = 831;
    update_cmd["group_meta"] = to_hex(meta_v2);

    auto update_str = send_cmd(admin_client, update_cmd, 5000);
    ASSERT_FALSE(update_str.empty());
    auto update_resp = parse_json(update_str);
    EXPECT_TRUE(update_resp["ok"].asBool()) << "GROUP_UPDATE failed: " << update_str;

    admin_client.close();
}

TEST_F(WsServerTest, GroupUpdateAdminCannotChangeRoles) {
    start_ws_server();

    auto owner_kp = crypto::generate_keypair();
    auto owner_fp = crypto::sha3_256(owner_kp.public_key);

    auto admin_kp = crypto::generate_keypair();
    auto admin_fp = crypto::sha3_256(admin_kp.public_key);

    auto member_kp = crypto::generate_keypair();
    auto member_fp = crypto::sha3_256(member_kp.public_key);

    crypto::Hash group_id{};
    for (auto& b : group_id) b = static_cast<uint8_t>(rand() & 0xFF);

    // Create group with owner + admin + member
    auto meta_v1 = build_group_meta(group_id, 1,
        {{owner_fp, 0x02}, {admin_fp, 0x01}, {member_fp, 0x00}}, owner_kp);

    TestWsClient owner_client;
    ASSERT_TRUE(owner_client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(owner_client, owner_kp));

    Json::Value create_cmd;
    create_cmd["type"] = "GROUP_CREATE";
    create_cmd["id"] = 840;
    create_cmd["group_meta"] = to_hex(meta_v1);

    auto create_str = send_cmd(owner_client, create_cmd, 5000);
    ASSERT_FALSE(create_str.empty());
    auto create_resp = parse_json(create_str);
    ASSERT_TRUE(create_resp["ok"].asBool()) << "GROUP_CREATE failed: " << create_str;
    owner_client.close();

    // Admin tries to promote member to admin — should fail
    TestWsClient admin_client;
    ASSERT_TRUE(admin_client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(admin_client, admin_kp));

    auto meta_v2 = build_group_meta(group_id, 2,
        {{owner_fp, 0x02}, {admin_fp, 0x01}, {member_fp, 0x01}}, owner_kp);

    Json::Value update_cmd;
    update_cmd["type"] = "GROUP_UPDATE";
    update_cmd["id"] = 841;
    update_cmd["group_meta"] = to_hex(meta_v2);

    auto update_str = send_cmd(admin_client, update_cmd, 5000);
    ASSERT_FALSE(update_str.empty());
    auto update_resp = parse_json(update_str);
    EXPECT_EQ(update_resp["type"].asString(), "ERROR");
    EXPECT_EQ(update_resp["code"].asInt(), 403);

    admin_client.close();
}

TEST_F(WsServerTest, GroupDestroy) {
    start_ws_server();

    auto owner_kp = crypto::generate_keypair();
    auto owner_fp = crypto::sha3_256(owner_kp.public_key);

    auto member_kp = crypto::generate_keypair();
    auto member_fp = crypto::sha3_256(member_kp.public_key);

    crypto::Hash group_id{};
    for (auto& b : group_id) b = static_cast<uint8_t>(rand() & 0xFF);

    // Create group with owner + member
    auto meta = build_group_meta(group_id, 1,
        {{owner_fp, 0x02}, {member_fp, 0x00}}, owner_kp);

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, owner_kp));

    Json::Value create_cmd;
    create_cmd["type"] = "GROUP_CREATE";
    create_cmd["id"] = 900;
    create_cmd["group_meta"] = to_hex(meta);

    auto create_str = send_cmd(client, create_cmd, 5000);
    ASSERT_FALSE(create_str.empty());
    auto create_resp = parse_json(create_str);
    ASSERT_TRUE(create_resp["ok"].asBool()) << "GROUP_CREATE failed: " << create_str;

    // Send a message to the group
    crypto::Hash msg_id{};
    for (auto& b : msg_id) b = static_cast<uint8_t>(rand() & 0xFF);
    std::vector<uint8_t> blob(16);
    for (auto& b : blob) b = static_cast<uint8_t>(rand() & 0xFF);

    Json::Value send_cmd_val;
    send_cmd_val["type"] = "GROUP_SEND";
    send_cmd_val["id"] = 901;
    send_cmd_val["group_id"] = to_hex(group_id);
    send_cmd_val["msg_id"] = to_hex(msg_id);
    send_cmd_val["gek_version"] = 1;
    send_cmd_val["blob"] = to_hex(blob);

    auto send_str = send_cmd(client, send_cmd_val, 5000);
    ASSERT_FALSE(send_str.empty());
    auto send_resp = parse_json(send_str);
    ASSERT_TRUE(send_resp["ok"].asBool()) << "GROUP_SEND failed: " << send_str;

    // GROUP_DESTROY as owner
    Json::Value destroy_cmd;
    destroy_cmd["type"] = "GROUP_DESTROY";
    destroy_cmd["id"] = 902;
    destroy_cmd["group_id"] = to_hex(group_id);

    auto destroy_str = send_cmd(client, destroy_cmd, 5000);
    ASSERT_FALSE(destroy_str.empty());
    auto destroy_resp = parse_json(destroy_str);
    EXPECT_EQ(destroy_resp["id"].asInt(), 902);
    EXPECT_TRUE(destroy_resp["ok"].asBool()) << "GROUP_DESTROY failed: " << destroy_str;

    // GROUP_INFO should fail now (group is gone)
    Json::Value info_cmd;
    info_cmd["type"] = "GROUP_INFO";
    info_cmd["id"] = 903;
    info_cmd["group_id"] = to_hex(group_id);

    auto info_str = send_cmd(client, info_cmd, 5000);
    ASSERT_FALSE(info_str.empty());
    auto info_resp = parse_json(info_str);
    EXPECT_EQ(info_resp["type"].asString(), "ERROR");
    // Group is gone so either 404 (not found) or 403 (not a member) is acceptable
    EXPECT_TRUE(info_resp["code"].asInt() == 404 || info_resp["code"].asInt() == 403)
        << "Expected 404 or 403, got: " << info_str;

    client.close();
}

TEST_F(WsServerTest, GroupDestroyNotOwner) {
    start_ws_server();

    auto owner_kp = crypto::generate_keypair();
    auto owner_fp = crypto::sha3_256(owner_kp.public_key);

    auto admin_kp = crypto::generate_keypair();
    auto admin_fp = crypto::sha3_256(admin_kp.public_key);

    auto member_kp = crypto::generate_keypair();
    auto member_fp = crypto::sha3_256(member_kp.public_key);

    crypto::Hash group_id{};
    for (auto& b : group_id) b = static_cast<uint8_t>(rand() & 0xFF);

    // Create group with owner + admin + member
    auto meta = build_group_meta(group_id, 1,
        {{owner_fp, 0x02}, {admin_fp, 0x01}, {member_fp, 0x00}}, owner_kp);

    TestWsClient owner_client;
    ASSERT_TRUE(owner_client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(owner_client, owner_kp));

    Json::Value create_cmd;
    create_cmd["type"] = "GROUP_CREATE";
    create_cmd["id"] = 910;
    create_cmd["group_meta"] = to_hex(meta);

    auto create_str = send_cmd(owner_client, create_cmd, 5000);
    ASSERT_FALSE(create_str.empty());
    auto create_resp = parse_json(create_str);
    ASSERT_TRUE(create_resp["ok"].asBool()) << "GROUP_CREATE failed: " << create_str;
    owner_client.close();

    // Admin tries GROUP_DESTROY — should fail with 403
    TestWsClient admin_client;
    ASSERT_TRUE(admin_client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(admin_client, admin_kp));

    Json::Value destroy_admin;
    destroy_admin["type"] = "GROUP_DESTROY";
    destroy_admin["id"] = 911;
    destroy_admin["group_id"] = to_hex(group_id);

    auto admin_str = send_cmd(admin_client, destroy_admin, 5000);
    ASSERT_FALSE(admin_str.empty());
    auto admin_resp = parse_json(admin_str);
    EXPECT_EQ(admin_resp["type"].asString(), "ERROR");
    EXPECT_EQ(admin_resp["code"].asInt(), 403);
    admin_client.close();

    // Member tries GROUP_DESTROY — should fail with 403
    TestWsClient member_client;
    ASSERT_TRUE(member_client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(member_client, member_kp));

    Json::Value destroy_member;
    destroy_member["type"] = "GROUP_DESTROY";
    destroy_member["id"] = 912;
    destroy_member["group_id"] = to_hex(group_id);

    auto member_str = send_cmd(member_client, destroy_member, 5000);
    ASSERT_FALSE(member_str.empty());
    auto member_resp = parse_json(member_str);
    EXPECT_EQ(member_resp["type"].asString(), "ERROR");
    EXPECT_EQ(member_resp["code"].asInt(), 403);
    member_client.close();
}
