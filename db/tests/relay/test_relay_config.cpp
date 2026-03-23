#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "relay/config/relay_config.h"
#include <fstream>
#include <filesystem>

using namespace chromatindb::relay::config;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("Load valid JSON config with all 6 fields", "[relay_config]") {
    auto tmp = std::filesystem::temp_directory_path() / "relay_test_full.json";
    {
        std::ofstream f(tmp);
        f << R"({
            "bind_address": "192.168.1.1",
            "bind_port": 8080,
            "uds_path": "/var/run/chromatindb.sock",
            "identity_key_path": "/etc/chromatindb/relay.key",
            "log_level": "debug",
            "log_file": "/var/log/relay.log"
        })";
    }

    auto cfg = load_relay_config(tmp);

    REQUIRE(cfg.bind_address == "192.168.1.1");
    REQUIRE(cfg.bind_port == 8080);
    REQUIRE(cfg.uds_path == "/var/run/chromatindb.sock");
    REQUIRE(cfg.identity_key_path == "/etc/chromatindb/relay.key");
    REQUIRE(cfg.log_level == "debug");
    REQUIRE(cfg.log_file == "/var/log/relay.log");

    std::filesystem::remove(tmp);
}

TEST_CASE("Load JSON with missing optional fields uses defaults", "[relay_config]") {
    auto tmp = std::filesystem::temp_directory_path() / "relay_test_partial.json";
    {
        std::ofstream f(tmp);
        f << R"({
            "uds_path": "/var/run/chromatindb.sock",
            "identity_key_path": "/etc/chromatindb/relay.key"
        })";
    }

    auto cfg = load_relay_config(tmp);

    REQUIRE(cfg.bind_address == "0.0.0.0");
    REQUIRE(cfg.bind_port == 4201);
    REQUIRE(cfg.log_level == "info");
    REQUIRE(cfg.log_file.empty());
    // Required fields present
    REQUIRE(cfg.uds_path == "/var/run/chromatindb.sock");
    REQUIRE(cfg.identity_key_path == "/etc/chromatindb/relay.key");

    std::filesystem::remove(tmp);
}

TEST_CASE("validate_relay_config rejects bind_port=0", "[relay_config]") {
    RelayConfig cfg;
    cfg.bind_port = 0;
    cfg.uds_path = "/var/run/chromatindb.sock";
    cfg.identity_key_path = "/etc/chromatindb/relay.key";

    REQUIRE_THROWS_AS(validate_relay_config(cfg), std::runtime_error);
    try {
        validate_relay_config(cfg);
    } catch (const std::runtime_error& e) {
        REQUIRE_THAT(e.what(), ContainsSubstring("bind_port"));
    }
}

TEST_CASE("validate_relay_config rejects bind_port=70000", "[relay_config]") {
    RelayConfig cfg;
    cfg.bind_port = 70000;
    cfg.uds_path = "/var/run/chromatindb.sock";
    cfg.identity_key_path = "/etc/chromatindb/relay.key";

    REQUIRE_THROWS_AS(validate_relay_config(cfg), std::runtime_error);
    try {
        validate_relay_config(cfg);
    } catch (const std::runtime_error& e) {
        REQUIRE_THAT(e.what(), ContainsSubstring("bind_port"));
    }
}

TEST_CASE("validate_relay_config rejects empty identity_key_path", "[relay_config]") {
    RelayConfig cfg;
    cfg.uds_path = "/var/run/chromatindb.sock";
    cfg.identity_key_path = "";

    REQUIRE_THROWS_AS(validate_relay_config(cfg), std::runtime_error);
    try {
        validate_relay_config(cfg);
    } catch (const std::runtime_error& e) {
        REQUIRE_THAT(e.what(), ContainsSubstring("identity_key_path"));
    }
}

TEST_CASE("validate_relay_config rejects empty uds_path", "[relay_config]") {
    RelayConfig cfg;
    cfg.uds_path = "";
    cfg.identity_key_path = "/etc/chromatindb/relay.key";

    REQUIRE_THROWS_AS(validate_relay_config(cfg), std::runtime_error);
    try {
        validate_relay_config(cfg);
    } catch (const std::runtime_error& e) {
        REQUIRE_THAT(e.what(), ContainsSubstring("uds_path"));
    }
}

TEST_CASE("validate_relay_config rejects identity_key_path not ending in .key", "[relay_config]") {
    RelayConfig cfg;
    cfg.uds_path = "/var/run/chromatindb.sock";
    cfg.identity_key_path = "/etc/chromatindb/relay.pem";

    REQUIRE_THROWS_AS(validate_relay_config(cfg), std::runtime_error);
    try {
        validate_relay_config(cfg);
    } catch (const std::runtime_error& e) {
        REQUIRE_THAT(e.what(), ContainsSubstring(".key"));
    }
}

TEST_CASE("validate_relay_config accumulates multiple errors", "[relay_config]") {
    RelayConfig cfg;
    cfg.bind_port = 0;
    cfg.uds_path = "";
    cfg.identity_key_path = "";

    REQUIRE_THROWS_AS(validate_relay_config(cfg), std::runtime_error);
    try {
        validate_relay_config(cfg);
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        REQUIRE_THAT(msg, ContainsSubstring("bind_port"));
        REQUIRE_THAT(msg, ContainsSubstring("uds_path"));
        REQUIRE_THAT(msg, ContainsSubstring("identity_key_path"));
    }
}

TEST_CASE("validate_relay_config accepts valid config without throwing", "[relay_config]") {
    RelayConfig cfg;
    cfg.bind_address = "0.0.0.0";
    cfg.bind_port = 4201;
    cfg.uds_path = "/var/run/chromatindb.sock";
    cfg.identity_key_path = "/etc/chromatindb/relay.key";
    cfg.log_level = "info";

    REQUIRE_NOTHROW(validate_relay_config(cfg));
}

TEST_CASE("load_relay_config throws on malformed JSON", "[relay_config]") {
    auto tmp = std::filesystem::temp_directory_path() / "relay_test_malformed.json";
    {
        std::ofstream f(tmp);
        f << "{ this is not valid json }}}";
    }

    REQUIRE_THROWS_AS(load_relay_config(tmp), std::runtime_error);
    try {
        load_relay_config(tmp);
    } catch (const std::runtime_error& e) {
        REQUIRE_THAT(e.what(), ContainsSubstring("JSON"));
    }

    std::filesystem::remove(tmp);
}

TEST_CASE("load_relay_config throws on nonexistent file", "[relay_config]") {
    REQUIRE_THROWS_AS(
        load_relay_config("/tmp/relay_nonexistent_config_xyz.json"),
        std::runtime_error
    );
    try {
        load_relay_config("/tmp/relay_nonexistent_config_xyz.json");
    } catch (const std::runtime_error& e) {
        REQUIRE_THAT(e.what(), ContainsSubstring("Config file not found"));
    }
}

TEST_CASE("validate_relay_config rejects invalid log_level", "[relay_config]") {
    RelayConfig cfg;
    cfg.uds_path = "/var/run/chromatindb.sock";
    cfg.identity_key_path = "/etc/chromatindb/relay.key";
    cfg.log_level = "verbose";

    REQUIRE_THROWS_AS(validate_relay_config(cfg), std::runtime_error);
    try {
        validate_relay_config(cfg);
    } catch (const std::runtime_error& e) {
        REQUIRE_THAT(e.what(), ContainsSubstring("log_level"));
    }
}

TEST_CASE("validate_relay_config rejects uds_path not starting with /", "[relay_config]") {
    RelayConfig cfg;
    cfg.uds_path = "relative/path/to.sock";
    cfg.identity_key_path = "/etc/chromatindb/relay.key";

    REQUIRE_THROWS_AS(validate_relay_config(cfg), std::runtime_error);
    try {
        validate_relay_config(cfg);
    } catch (const std::runtime_error& e) {
        REQUIRE_THAT(e.what(), ContainsSubstring("uds_path"));
        REQUIRE_THAT(e.what(), ContainsSubstring("absolute"));
    }
}

TEST_CASE("validate_relay_config rejects uds_path exceeding 107 chars", "[relay_config]") {
    RelayConfig cfg;
    cfg.uds_path = "/" + std::string(107, 'a');  // 108 chars total
    cfg.identity_key_path = "/etc/chromatindb/relay.key";

    REQUIRE_THROWS_AS(validate_relay_config(cfg), std::runtime_error);
    try {
        validate_relay_config(cfg);
    } catch (const std::runtime_error& e) {
        REQUIRE_THAT(e.what(), ContainsSubstring("uds_path"));
        REQUIRE_THAT(e.what(), ContainsSubstring("107"));
    }
}

TEST_CASE("validate_relay_config rejects empty bind_address", "[relay_config]") {
    RelayConfig cfg;
    cfg.bind_address = "";
    cfg.uds_path = "/var/run/chromatindb.sock";
    cfg.identity_key_path = "/etc/chromatindb/relay.key";

    REQUIRE_THROWS_AS(validate_relay_config(cfg), std::runtime_error);
    try {
        validate_relay_config(cfg);
    } catch (const std::runtime_error& e) {
        REQUIRE_THAT(e.what(), ContainsSubstring("bind_address"));
    }
}
