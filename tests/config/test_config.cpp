#include <catch2/catch_test_macros.hpp>
#include "config/config.h"
#include <fstream>
#include <filesystem>

using namespace chromatin::config;

TEST_CASE("Default config has sensible values", "[config]") {
    Config cfg;

    REQUIRE(cfg.bind_address == "0.0.0.0:4200");
    REQUIRE(cfg.storage_path == "./data/blobs");
    REQUIRE(cfg.data_dir == "./data");
    REQUIRE(cfg.bootstrap_peers.empty());
    REQUIRE(cfg.default_ttl == 604800);  // 7 days
    REQUIRE(cfg.log_level == "info");
}

TEST_CASE("load_config with non-existent file returns defaults", "[config]") {
    auto cfg = load_config("/tmp/chromatindb_test_nonexistent_config_12345.json");

    REQUIRE(cfg.bind_address == "0.0.0.0:4200");
    REQUIRE(cfg.storage_path == "./data/blobs");
    REQUIRE(cfg.data_dir == "./data");
    REQUIRE(cfg.bootstrap_peers.empty());
    REQUIRE(cfg.default_ttl == 604800);
    REQUIRE(cfg.log_level == "info");
}

TEST_CASE("load_config with valid JSON populates all fields", "[config]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_config.json";
    {
        std::ofstream f(tmp);
        f << R"({
            "bind_address": "127.0.0.1:5000",
            "storage_path": "/var/lib/chromatindb/blobs",
            "data_dir": "/var/lib/chromatindb",
            "bootstrap_peers": ["10.0.0.1:4200", "10.0.0.2:4200"],
            "default_ttl": 86400,
            "log_level": "debug"
        })";
    }

    auto cfg = load_config(tmp);

    REQUIRE(cfg.bind_address == "127.0.0.1:5000");
    REQUIRE(cfg.storage_path == "/var/lib/chromatindb/blobs");
    REQUIRE(cfg.data_dir == "/var/lib/chromatindb");
    REQUIRE(cfg.bootstrap_peers.size() == 2);
    REQUIRE(cfg.bootstrap_peers[0] == "10.0.0.1:4200");
    REQUIRE(cfg.bootstrap_peers[1] == "10.0.0.2:4200");
    REQUIRE(cfg.default_ttl == 86400);
    REQUIRE(cfg.log_level == "debug");

    std::filesystem::remove(tmp);
}

TEST_CASE("load_config with partial JSON uses defaults for missing fields", "[config]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_partial.json";
    {
        std::ofstream f(tmp);
        f << R"({"bind_address": "0.0.0.0:9999", "log_level": "trace"})";
    }

    auto cfg = load_config(tmp);

    REQUIRE(cfg.bind_address == "0.0.0.0:9999");
    REQUIRE(cfg.log_level == "trace");
    // Defaults for missing fields
    REQUIRE(cfg.storage_path == "./data/blobs");
    REQUIRE(cfg.data_dir == "./data");
    REQUIRE(cfg.bootstrap_peers.empty());
    REQUIRE(cfg.default_ttl == 604800);

    std::filesystem::remove(tmp);
}

TEST_CASE("load_config with invalid JSON throws runtime_error", "[config]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_invalid.json";
    {
        std::ofstream f(tmp);
        f << "{ this is not valid json }}}";
    }

    REQUIRE_THROWS_AS(load_config(tmp), std::runtime_error);

    std::filesystem::remove(tmp);
}

TEST_CASE("parse_args with --config loads from file", "[config]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_args.json";
    {
        std::ofstream f(tmp);
        f << R"({"bind_address": "192.168.1.1:4200", "default_ttl": 3600})";
    }

    const char* argv[] = {"chromatindb", "--config", tmp.c_str()};
    auto cfg = parse_args(3, argv);

    REQUIRE(cfg.bind_address == "192.168.1.1:4200");
    REQUIRE(cfg.default_ttl == 3600);

    std::filesystem::remove(tmp);
}

TEST_CASE("parse_args CLI overrides take precedence over config file", "[config]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_override.json";
    {
        std::ofstream f(tmp);
        f << R"({"data_dir": "/from/config", "log_level": "warn"})";
    }

    const char* argv[] = {"chromatindb", "--config", tmp.c_str(),
                           "--data-dir", "/from/cli", "--log-level", "error"};
    auto cfg = parse_args(7, argv);

    REQUIRE(cfg.data_dir == "/from/cli");
    REQUIRE(cfg.log_level == "error");

    std::filesystem::remove(tmp);
}

TEST_CASE("parse_args without --config uses base config", "[config]") {
    Config base;
    base.bind_address = "base_address";

    const char* argv[] = {"chromatindb", "--data-dir", "/override"};
    auto cfg = parse_args(3, argv, base);

    REQUIRE(cfg.bind_address == "base_address");
    REQUIRE(cfg.data_dir == "/override");
}
