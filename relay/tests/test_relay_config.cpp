#include <catch2/catch_test_macros.hpp>

#include "relay/config/relay_config.h"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

using chromatindb::relay::config::RelayConfig;
using chromatindb::relay::config::load_relay_config;
using chromatindb::relay::config::validate_relay_config;

namespace {

/// Helper: write a JSON object to a temp file and return the path.
class TempConfig {
public:
    explicit TempConfig(const nlohmann::json& j) {
        path_ = std::filesystem::temp_directory_path() / ("relay_test_" + std::to_string(counter_++) + ".json");
        std::ofstream f(path_);
        f << j.dump(2);
    }

    ~TempConfig() {
        std::filesystem::remove(path_);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    static inline int counter_ = 0;
};

nlohmann::json valid_config() {
    return {
        {"uds_path", "/tmp/chromatindb.sock"},
        {"identity_key_path", "/tmp/relay.key"},
        {"bind_address", "127.0.0.1"},
        {"bind_port", 8080},
        {"log_level", "debug"},
        {"log_file", "/tmp/relay.log"},
        {"max_send_queue", 512}
    };
}

} // namespace

TEST_CASE("Config: load valid JSON config populates all fields", "[config]") {
    TempConfig tc(valid_config());
    auto cfg = load_relay_config(tc.path());

    REQUIRE(cfg.uds_path == "/tmp/chromatindb.sock");
    REQUIRE(cfg.identity_key_path == "/tmp/relay.key");
    REQUIRE(cfg.bind_address == "127.0.0.1");
    REQUIRE(cfg.bind_port == 8080);
    REQUIRE(cfg.log_level == "debug");
    REQUIRE(cfg.log_file == "/tmp/relay.log");
    REQUIRE(cfg.max_send_queue == 512);
}

TEST_CASE("Config: missing required field uds_path throws", "[config]") {
    auto j = valid_config();
    j.erase("uds_path");
    TempConfig tc(j);

    REQUIRE_THROWS_AS(load_relay_config(tc.path()), std::runtime_error);
}

TEST_CASE("Config: missing required field identity_key_path throws", "[config]") {
    auto j = valid_config();
    j.erase("identity_key_path");
    TempConfig tc(j);

    REQUIRE_THROWS_AS(load_relay_config(tc.path()), std::runtime_error);
}

TEST_CASE("Config: load with defaults fills optional fields", "[config]") {
    nlohmann::json j = {
        {"uds_path", "/tmp/node.sock"},
        {"identity_key_path", "/tmp/relay.key"}
    };
    TempConfig tc(j);
    auto cfg = load_relay_config(tc.path());

    REQUIRE(cfg.bind_address == "0.0.0.0");
    REQUIRE(cfg.bind_port == 4201);
    REQUIRE(cfg.max_send_queue == 256);
    REQUIRE(cfg.log_level == "info");
    REQUIRE(cfg.log_file.empty());
}

TEST_CASE("Config: load nonexistent file throws", "[config]") {
    REQUIRE_THROWS_AS(load_relay_config("/nonexistent/relay.json"), std::runtime_error);
}

TEST_CASE("Config: validate bind_port=0 throws", "[config]") {
    RelayConfig cfg;
    cfg.uds_path = "/tmp/node.sock";
    cfg.identity_key_path = "/tmp/relay.key";
    cfg.bind_port = 0;

    REQUIRE_THROWS_AS(validate_relay_config(cfg), std::runtime_error);
}

TEST_CASE("Config: validate bind_port=65536 throws", "[config]") {
    RelayConfig cfg;
    cfg.uds_path = "/tmp/node.sock";
    cfg.identity_key_path = "/tmp/relay.key";
    cfg.bind_port = 65536;

    REQUIRE_THROWS_AS(validate_relay_config(cfg), std::runtime_error);
}

TEST_CASE("Config: load with cert_path and key_path populates TLS fields", "[config]") {
    auto j = valid_config();
    j["cert_path"] = "/tmp/test_cert.pem";
    j["key_path"] = "/tmp/test_key.pem";
    TempConfig tc(j);
    auto cfg = load_relay_config(tc.path());

    REQUIRE(cfg.cert_path == "/tmp/test_cert.pem");
    REQUIRE(cfg.key_path == "/tmp/test_key.pem");
}

TEST_CASE("Config: load without cert_path and key_path defaults to empty", "[config]") {
    TempConfig tc(valid_config());
    auto cfg = load_relay_config(tc.path());

    REQUIRE(cfg.cert_path.empty());
    REQUIRE(cfg.key_path.empty());
}

TEST_CASE("Config: tls_enabled returns true when both set, false when empty", "[config]") {
    RelayConfig cfg;
    cfg.uds_path = "/tmp/node.sock";
    cfg.identity_key_path = "/tmp/relay.key";

    REQUIRE(cfg.tls_enabled() == false);

    cfg.cert_path = "/tmp/cert.pem";
    cfg.key_path = "/tmp/key.pem";
    REQUIRE(cfg.tls_enabled() == true);
}

TEST_CASE("Config: validate with cert_path set but key_path empty throws", "[config]") {
    RelayConfig cfg;
    cfg.uds_path = "/tmp/node.sock";
    cfg.identity_key_path = "/tmp/relay.key";
    cfg.cert_path = "/tmp/cert.pem";
    // key_path is empty

    REQUIRE_THROWS_AS(validate_relay_config(cfg), std::runtime_error);
}

TEST_CASE("Config: validate with nonexistent cert_path throws", "[config]") {
    RelayConfig cfg;
    cfg.uds_path = "/tmp/node.sock";
    cfg.identity_key_path = "/tmp/relay.key";
    cfg.cert_path = "/nonexistent/cert.pem";
    cfg.key_path = "/nonexistent/key.pem";

    REQUIRE_THROWS_AS(validate_relay_config(cfg), std::runtime_error);
}
