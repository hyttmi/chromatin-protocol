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
    std::unique_ptr<ws::WsServer> server_;
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

        kademlia_ = std::make_unique<kademlia::Kademlia>(
            self, *transport_, *routing_table_, *storage_, *repl_log_, node_keypair_);
        kademlia_->set_name_pow_difficulty(8);

        // Start TCP accept thread
        tcp_thread_ = std::thread([this]() {
            transport_->run([this](const kademlia::Message& msg,
                                  const std::string& from, uint16_t port) {
                kademlia_->handle_message(msg, from, port);
            });
        });

        cfg_.bind = "127.0.0.1";
        cfg_.ws_port = 0;  // ephemeral
        cfg_.data_dir = db_path_;
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

        // Sign the nonce
        std::string nonce_hex = challenge["nonce"].asString();
        auto nonce_bytes = from_hex(nonce_hex);
        if (nonce_bytes.size() != 32) return false;

        auto signature = crypto::sign(nonce_bytes, user_kp.secret_key);

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
        server_ = std::make_unique<ws::WsServer>(
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

    // Step 2: Sign the nonce
    std::string nonce_hex = challenge["nonce"].asString();
    auto nonce_bytes = from_hex(nonce_hex);
    ASSERT_EQ(nonce_bytes.size(), 32u);

    auto signature = crypto::sign(nonce_bytes, user_kp.secret_key);
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

    // Try FETCH without authenticating
    ASSERT_TRUE(client.send_text(R"({"type":"FETCH","id":7})"));

    auto resp = client.recv_text();
    ASSERT_TRUE(resp.has_value()) << "no response to unauthenticated FETCH";

    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "ERROR");
    EXPECT_EQ(root["id"].asInt(), 7);
    EXPECT_EQ(root["code"].asInt(), 401);

    client.close();
}

// ---------- FETCH tests ----------

TEST_F(WsServerTest, FetchEmptyInbox) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    // FETCH with no messages in inbox
    ASSERT_TRUE(client.send_text(R"({"type":"FETCH","id":10})"));

    auto resp = client.recv_text();
    ASSERT_TRUE(resp.has_value()) << "no response to FETCH";

    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "MESSAGES");
    EXPECT_EQ(root["id"].asInt(), 10);
    ASSERT_TRUE(root["messages"].isArray());
    EXPECT_EQ(root["messages"].size(), 0u);

    client.close();
}

TEST_F(WsServerTest, FetchWithMessages) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();
    auto fingerprint = crypto::sha3_256(user_kp.public_key);

    // Manually insert a message into TABLE_INBOXES before connecting.
    // Key layout: recipient_fp(32) || timestamp(8 BE) || msg_id(32) = 72 bytes
    // Value layout: msg_id(32) || sender_fp(32) || timestamp(8) || blob_len(4 BE) || blob

    // Create a fake msg_id (32 bytes)
    crypto::Hash msg_id{};
    msg_id.fill(0xAA);

    // Create a fake sender fingerprint (32 bytes)
    crypto::Hash sender_fp{};
    sender_fp.fill(0xBB);

    // Timestamp
    uint64_t timestamp = 1700000000;

    // Build key: fp(32) || timestamp(8 BE) || msg_id(32)
    std::vector<uint8_t> key;
    key.insert(key.end(), fingerprint.begin(), fingerprint.end());
    for (int i = 7; i >= 0; --i) {
        key.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    }
    key.insert(key.end(), msg_id.begin(), msg_id.end());
    ASSERT_EQ(key.size(), 72u);

    // Build value: msg_id(32) || sender_fp(32) || timestamp(8) || blob_len(4 BE) || blob
    std::vector<uint8_t> blob = {0x48, 0x65, 0x6C, 0x6C, 0x6F};  // "Hello"
    std::vector<uint8_t> value;
    value.insert(value.end(), msg_id.begin(), msg_id.end());
    value.insert(value.end(), sender_fp.begin(), sender_fp.end());
    for (int i = 7; i >= 0; --i) {
        value.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    }
    uint32_t blob_len = static_cast<uint32_t>(blob.size());
    value.push_back(static_cast<uint8_t>((blob_len >> 24) & 0xFF));
    value.push_back(static_cast<uint8_t>((blob_len >> 16) & 0xFF));
    value.push_back(static_cast<uint8_t>((blob_len >> 8) & 0xFF));
    value.push_back(static_cast<uint8_t>(blob_len & 0xFF));
    value.insert(value.end(), blob.begin(), blob.end());

    storage_->put(storage::TABLE_INBOXES, key, value);

    // Now connect and authenticate
    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    // FETCH
    ASSERT_TRUE(client.send_text(R"({"type":"FETCH","id":20})"));

    auto resp = client.recv_text();
    ASSERT_TRUE(resp.has_value()) << "no response to FETCH";

    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "MESSAGES");
    EXPECT_EQ(root["id"].asInt(), 20);
    ASSERT_TRUE(root["messages"].isArray());
    ASSERT_EQ(root["messages"].size(), 1u);

    auto& entry = root["messages"][0];
    EXPECT_EQ(entry["msg_id"].asString(), to_hex(msg_id));
    EXPECT_EQ(entry["from"].asString(), to_hex(sender_fp));
    EXPECT_EQ(entry["timestamp"].asUInt64(), timestamp);
    // blob "Hello" -> base64 "SGVsbG8="
    EXPECT_EQ(entry["blob"].asString(), "SGVsbG8=");

    client.close();
}
