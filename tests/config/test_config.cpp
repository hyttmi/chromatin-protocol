#include <catch2/catch_test_macros.hpp>
#include "db/config/config.h"
#include <fstream>
#include <filesystem>

using namespace chromatindb::config;

TEST_CASE("Default config has sensible values", "[config]") {
    Config cfg;

    REQUIRE(cfg.bind_address == "0.0.0.0:4200");
    REQUIRE(cfg.storage_path == "./data/blobs");
    REQUIRE(cfg.data_dir == "./data");
    REQUIRE(cfg.bootstrap_peers.empty());
    REQUIRE(cfg.log_level == "info");
}

TEST_CASE("load_config with non-existent file returns defaults", "[config]") {
    auto cfg = load_config("/tmp/chromatindb_test_nonexistent_config_12345.json");

    REQUIRE(cfg.bind_address == "0.0.0.0:4200");
    REQUIRE(cfg.storage_path == "./data/blobs");
    REQUIRE(cfg.data_dir == "./data");
    REQUIRE(cfg.bootstrap_peers.empty());
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
        f << R"({"bind_address": "192.168.1.1:4200"})";
    }

    const char* argv[] = {"chromatindb", "--config", tmp.c_str()};
    auto cfg = parse_args(3, argv);

    REQUIRE(cfg.bind_address == "192.168.1.1:4200");

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

// =============================================================================
// Storage capacity: max_storage_bytes config tests
// =============================================================================

TEST_CASE("max_storage_bytes parses from JSON", "[config][capacity]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_maxstorage.json";
    {
        std::ofstream f(tmp);
        f << R"({"max_storage_bytes": 1073741824})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.max_storage_bytes == 1073741824);  // 1 GiB

    std::filesystem::remove(tmp);
}

TEST_CASE("max_storage_bytes defaults to 0 when missing", "[config][capacity]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_nomaxstorage.json";
    {
        std::ofstream f(tmp);
        f << R"({"bind_address": "0.0.0.0:4200"})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.max_storage_bytes == 0);

    std::filesystem::remove(tmp);
}

TEST_CASE("Default config has max_storage_bytes 0 (unlimited)", "[config][capacity]") {
    Config cfg;
    REQUIRE(cfg.max_storage_bytes == 0);
}

// =============================================================================
// ACL: allowed_keys config tests
// =============================================================================

TEST_CASE("Default config has empty allowed_keys", "[config][acl]") {
    Config cfg;
    REQUIRE(cfg.allowed_keys.empty());
    REQUIRE(cfg.config_path.empty());
}

TEST_CASE("load_config parses allowed_keys from JSON array", "[config][acl]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_acl.json";
    {
        std::ofstream f(tmp);
        f << R"({
            "allowed_keys": [
                "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2",
                "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef"
            ]
        })";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.allowed_keys.size() == 2);
    REQUIRE(cfg.allowed_keys[0] == "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2");
    REQUIRE(cfg.allowed_keys[1] == "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef");

    std::filesystem::remove(tmp);
}

TEST_CASE("load_config with missing allowed_keys results in empty vector", "[config][acl]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_no_acl.json";
    {
        std::ofstream f(tmp);
        f << R"({"bind_address": "0.0.0.0:4200"})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.allowed_keys.empty());

    std::filesystem::remove(tmp);
}

TEST_CASE("load_config with empty allowed_keys array results in empty vector", "[config][acl]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_empty_acl.json";
    {
        std::ofstream f(tmp);
        f << R"({"allowed_keys": []})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.allowed_keys.empty());

    std::filesystem::remove(tmp);
}

TEST_CASE("load_config throws on key with wrong length", "[config][acl]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_bad_len.json";

    SECTION("too short (63 chars)") {
        {
            std::ofstream f(tmp);
            f << R"({"allowed_keys": ["a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b"]})";
        }
        REQUIRE_THROWS_AS(load_config(tmp), std::runtime_error);
    }

    SECTION("too long (65 chars)") {
        {
            std::ofstream f(tmp);
            f << R"({"allowed_keys": ["a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2a"]})";
        }
        REQUIRE_THROWS_AS(load_config(tmp), std::runtime_error);
    }

    std::filesystem::remove(tmp);
}

TEST_CASE("load_config throws on key with non-hex characters", "[config][acl]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_bad_hex.json";
    {
        std::ofstream f(tmp);
        // 'g' is not a hex char
        f << R"({"allowed_keys": ["g1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2"]})";
    }

    REQUIRE_THROWS_AS(load_config(tmp), std::runtime_error);

    std::filesystem::remove(tmp);
}

TEST_CASE("load_config accepts uppercase hex in allowed_keys", "[config][acl]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_upper.json";
    {
        std::ofstream f(tmp);
        f << R"({"allowed_keys": ["A1B2C3D4E5F6A1B2C3D4E5F6A1B2C3D4E5F6A1B2C3D4E5F6A1B2C3D4E5F6A1B2"]})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.allowed_keys.size() == 1);

    std::filesystem::remove(tmp);
}

TEST_CASE("parse_args stores config_path when --config provided", "[config][acl]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_path.json";
    {
        std::ofstream f(tmp);
        f << R"({})";
    }

    const char* argv[] = {"chromatindb", "--config", tmp.c_str()};
    auto cfg = parse_args(3, argv);

    REQUIRE(cfg.config_path == tmp);

    std::filesystem::remove(tmp);
}

TEST_CASE("parse_args leaves config_path empty when --config not provided", "[config][acl]") {
    const char* argv[] = {"chromatindb", "--data-dir", "/some/dir"};
    auto cfg = parse_args(3, argv);

    REQUIRE(cfg.config_path.empty());
}

TEST_CASE("validate_allowed_keys accepts valid keys", "[config][acl]") {
    std::vector<std::string> valid_keys = {
        "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2",
        "AABBCCDD00112233AABBCCDD00112233AABBCCDD00112233AABBCCDD00112233"
    };
    REQUIRE_NOTHROW(validate_allowed_keys(valid_keys));
}

TEST_CASE("validate_allowed_keys accepts empty vector", "[config][acl]") {
    std::vector<std::string> empty;
    REQUIRE_NOTHROW(validate_allowed_keys(empty));
}

TEST_CASE("validate_allowed_keys rejects malformed keys", "[config][acl]") {
    SECTION("wrong length") {
        std::vector<std::string> keys = {"abcd"};
        REQUIRE_THROWS_AS(validate_allowed_keys(keys), std::runtime_error);
    }

    SECTION("non-hex chars") {
        std::vector<std::string> keys = {"zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"};
        REQUIRE_THROWS_AS(validate_allowed_keys(keys), std::runtime_error);
    }
}

// =============================================================================
// Rate limiting: config field tests
// =============================================================================

TEST_CASE("Default config has rate_limit_bytes_per_sec 0 (disabled)", "[config][ratelimit]") {
    Config cfg;
    REQUIRE(cfg.rate_limit_bytes_per_sec == 0);
    REQUIRE(cfg.rate_limit_burst == 0);
}

TEST_CASE("rate_limit_bytes_per_sec and rate_limit_burst parse from JSON", "[config][ratelimit]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_ratelimit.json";
    {
        std::ofstream f(tmp);
        f << R"({"rate_limit_bytes_per_sec": 1048576, "rate_limit_burst": 10485760})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.rate_limit_bytes_per_sec == 1048576);   // 1 MiB/s
    REQUIRE(cfg.rate_limit_burst == 10485760);           // 10 MiB

    std::filesystem::remove(tmp);
}

TEST_CASE("rate_limit fields default to 0 when missing from JSON", "[config][ratelimit]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_no_ratelimit.json";
    {
        std::ofstream f(tmp);
        f << R"({"bind_address": "0.0.0.0:4200"})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.rate_limit_bytes_per_sec == 0);
    REQUIRE(cfg.rate_limit_burst == 0);

    std::filesystem::remove(tmp);
}

// =============================================================================
// Namespace filtering: sync_namespaces config tests
// =============================================================================

TEST_CASE("Default config has empty sync_namespaces", "[config][nsfilter]") {
    Config cfg;
    REQUIRE(cfg.sync_namespaces.empty());
}

TEST_CASE("load_config parses sync_namespaces from JSON array", "[config][nsfilter]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_nsfilter.json";
    {
        std::ofstream f(tmp);
        f << R"({
            "sync_namespaces": [
                "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2",
                "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef"
            ]
        })";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.sync_namespaces.size() == 2);
    REQUIRE(cfg.sync_namespaces[0] == "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2");
    REQUIRE(cfg.sync_namespaces[1] == "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef");

    std::filesystem::remove(tmp);
}

TEST_CASE("sync_namespaces defaults to empty when missing from JSON", "[config][nsfilter]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_no_nsfilter.json";
    {
        std::ofstream f(tmp);
        f << R"({"bind_address": "0.0.0.0:4200"})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.sync_namespaces.empty());

    std::filesystem::remove(tmp);
}

// =============================================================================
// Transport optimization: trusted_peers config tests
// =============================================================================

TEST_CASE("Default config has empty trusted_peers", "[config][transport]") {
    Config cfg;
    REQUIRE(cfg.trusted_peers.empty());
}

TEST_CASE("load_config parses trusted_peers from JSON array", "[config][transport]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_trusted.json";
    {
        std::ofstream f(tmp);
        f << R"({
            "trusted_peers": ["192.168.1.5", "10.0.0.1", "::1", "fe80::1"]
        })";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.trusted_peers.size() == 4);
    REQUIRE(cfg.trusted_peers[0] == "192.168.1.5");
    REQUIRE(cfg.trusted_peers[1] == "10.0.0.1");
    REQUIRE(cfg.trusted_peers[2] == "::1");
    REQUIRE(cfg.trusted_peers[3] == "fe80::1");

    std::filesystem::remove(tmp);
}

TEST_CASE("load_config without trusted_peers returns empty vector", "[config][transport]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_no_trusted.json";
    {
        std::ofstream f(tmp);
        f << R"({"bind_address": "0.0.0.0:4200"})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.trusted_peers.empty());

    std::filesystem::remove(tmp);
}

TEST_CASE("validate_trusted_peers accepts valid IPv4 addresses", "[config][transport]") {
    std::vector<std::string> peers = {"192.168.1.5", "10.0.0.1", "127.0.0.1"};
    REQUIRE_NOTHROW(validate_trusted_peers(peers));
}

TEST_CASE("validate_trusted_peers accepts valid IPv6 addresses", "[config][transport]") {
    std::vector<std::string> peers = {"::1", "fe80::1", "2001:db8::1"};
    REQUIRE_NOTHROW(validate_trusted_peers(peers));
}

TEST_CASE("validate_trusted_peers accepts empty list", "[config][transport]") {
    std::vector<std::string> peers;
    REQUIRE_NOTHROW(validate_trusted_peers(peers));
}

TEST_CASE("validate_trusted_peers rejects IPv4 with port", "[config][transport]") {
    std::vector<std::string> peers = {"192.168.1.5:4200"};
    REQUIRE_THROWS_AS(validate_trusted_peers(peers), std::runtime_error);
    try {
        validate_trusted_peers(peers);
    } catch (const std::runtime_error& e) {
        // Should mention port removal in error message
        std::string msg = e.what();
        REQUIRE(msg.find("port") != std::string::npos);
    }
}

TEST_CASE("validate_trusted_peers rejects IPv6 with port", "[config][transport]") {
    std::vector<std::string> peers = {"[::1]:4200"};
    REQUIRE_THROWS_AS(validate_trusted_peers(peers), std::runtime_error);
}

TEST_CASE("validate_trusted_peers rejects non-IP strings", "[config][transport]") {
    std::vector<std::string> peers = {"not-an-ip"};
    REQUIRE_THROWS_AS(validate_trusted_peers(peers), std::runtime_error);
}

TEST_CASE("load_config throws on invalid trusted_peers entries", "[config][transport]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_bad_trusted.json";

    SECTION("IPv4 with port") {
        {
            std::ofstream f(tmp);
            f << R"({"trusted_peers": ["192.168.1.5:4200"]})";
        }
        REQUIRE_THROWS_AS(load_config(tmp), std::runtime_error);
    }

    SECTION("non-IP string") {
        {
            std::ofstream f(tmp);
            f << R"({"trusted_peers": ["not-an-ip"]})";
        }
        REQUIRE_THROWS_AS(load_config(tmp), std::runtime_error);
    }

    std::filesystem::remove(tmp);
}

TEST_CASE("load_config throws on sync_namespaces with invalid entries", "[config][nsfilter]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_bad_nsfilter.json";

    SECTION("wrong length") {
        {
            std::ofstream f(tmp);
            f << R"({"sync_namespaces": ["abcd"]})";
        }
        REQUIRE_THROWS_AS(load_config(tmp), std::runtime_error);
    }

    SECTION("non-hex chars") {
        {
            std::ofstream f(tmp);
            f << R"({"sync_namespaces": ["zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"]})";
        }
        REQUIRE_THROWS_AS(load_config(tmp), std::runtime_error);
    }

    std::filesystem::remove(tmp);
}
