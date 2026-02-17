#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "config/config.h"

namespace fs = std::filesystem;
using helix::config::Config;

class ConfigTest : public ::testing::Test {
protected:
    fs::path tmp_dir_;

    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() / ("helix_config_test_" + std::to_string(::getpid()));
        fs::create_directories(tmp_dir_);
    }

    void TearDown() override {
        fs::remove_all(tmp_dir_);
    }

    void write_file(const fs::path& path, const std::string& content) {
        std::ofstream ofs(path);
        ofs << content;
    }
};

TEST_F(ConfigTest, ParseValidConfig) {
    auto path = tmp_dir_ / "config.json";
    write_file(path, R"({
        "data_dir": "/tmp/helix-data",
        "bind": "127.0.0.1",
        "udp_port": 5000,
        "ws_port": 5001,
        "bootstrap": ["node1.example.com:4000", "node2.example.com:4001"]
    })");

    auto cfg = helix::config::load_config(path);
    EXPECT_EQ(cfg.data_dir, "/tmp/helix-data");
    EXPECT_EQ(cfg.bind, "127.0.0.1");
    EXPECT_EQ(cfg.udp_port, 5000);
    EXPECT_EQ(cfg.ws_port, 5001);
    ASSERT_EQ(cfg.bootstrap.size(), 2);
    EXPECT_EQ(cfg.bootstrap[0].first, "node1.example.com");
    EXPECT_EQ(cfg.bootstrap[0].second, 4000);
    EXPECT_EQ(cfg.bootstrap[1].first, "node2.example.com");
    EXPECT_EQ(cfg.bootstrap[1].second, 4001);
}

TEST_F(ConfigTest, MissingFieldsUseDefaults) {
    auto path = tmp_dir_ / "config.json";
    write_file(path, "{}");

    auto cfg = helix::config::load_config(path);
    EXPECT_EQ(cfg.data_dir, ".");
    EXPECT_EQ(cfg.bind, "0.0.0.0");
    EXPECT_EQ(cfg.udp_port, 4000);
    EXPECT_EQ(cfg.ws_port, 4001);
    EXPECT_TRUE(cfg.bootstrap.empty());
}

TEST_F(ConfigTest, GenerateTemplateAndReparse) {
    auto path = tmp_dir_ / "generated.json";
    helix::config::generate_default_config(path);
    ASSERT_TRUE(fs::exists(path));

    auto cfg = helix::config::load_config(path);
    EXPECT_EQ(cfg.data_dir, ".");
    EXPECT_EQ(cfg.bind, "0.0.0.0");
    EXPECT_EQ(cfg.udp_port, 4000);
    EXPECT_EQ(cfg.ws_port, 4001);
    ASSERT_EQ(cfg.bootstrap.size(), 3);
    EXPECT_EQ(cfg.bootstrap[0].first, "0.bootstrap.cpunk.io");
    EXPECT_EQ(cfg.bootstrap[0].second, 4000);
}

TEST_F(ConfigTest, InvalidJsonThrows) {
    auto path = tmp_dir_ / "bad.json";
    write_file(path, "not json at all {{{");

    EXPECT_THROW(helix::config::load_config(path), std::runtime_error);
}

TEST_F(ConfigTest, MissingFileThrows) {
    EXPECT_THROW(helix::config::load_config(tmp_dir_ / "nonexistent.json"), std::runtime_error);
}

TEST_F(ConfigTest, InvalidBootstrapEndpointThrows) {
    auto path = tmp_dir_ / "config.json";
    write_file(path, R"({"bootstrap": ["no-port"]})");

    EXPECT_THROW(helix::config::load_config(path), std::runtime_error);
}

TEST_F(ConfigTest, KeypairRoundTrip) {
    auto kp1 = helix::config::load_or_generate_keypair(tmp_dir_);
    ASSERT_TRUE(fs::exists(tmp_dir_ / "node.key"));
    EXPECT_EQ(kp1.public_key.size(), helix::crypto::PUBLIC_KEY_SIZE);
    EXPECT_EQ(kp1.secret_key.size(), helix::crypto::SECRET_KEY_SIZE);

    // Load again — should get identical keys
    auto kp2 = helix::config::load_or_generate_keypair(tmp_dir_);
    EXPECT_EQ(kp1.public_key, kp2.public_key);
    EXPECT_EQ(kp1.secret_key, kp2.secret_key);
}
