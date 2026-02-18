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

#include <filesystem>
#include <sstream>
#include <thread>

#include <json/json.h>

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
