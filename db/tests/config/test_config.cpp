#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
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
// ACL: allowed_client_keys / allowed_peer_keys config tests
// =============================================================================

TEST_CASE("Default config has empty allowed_client_keys and allowed_peer_keys", "[config][acl]") {
    Config cfg;
    REQUIRE(cfg.allowed_client_keys.empty());
    REQUIRE(cfg.allowed_peer_keys.empty());
    REQUIRE(cfg.config_path.empty());
}

TEST_CASE("load_config parses allowed_client_keys from JSON array", "[config][acl]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_acl_client.json";
    {
        std::ofstream f(tmp);
        f << R"({
            "allowed_client_keys": [
                "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2",
                "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef"
            ]
        })";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.allowed_client_keys.size() == 2);
    REQUIRE(cfg.allowed_client_keys[0] == "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2");
    REQUIRE(cfg.allowed_client_keys[1] == "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef");

    std::filesystem::remove(tmp);
}

TEST_CASE("load_config parses allowed_peer_keys from JSON array", "[config][acl]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_acl_peer.json";
    {
        std::ofstream f(tmp);
        f << R"({
            "allowed_peer_keys": [
                "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2"
            ]
        })";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.allowed_peer_keys.size() == 1);
    REQUIRE(cfg.allowed_peer_keys[0] == "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2");

    std::filesystem::remove(tmp);
}

TEST_CASE("load_config with missing key lists results in empty vectors", "[config][acl]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_no_acl.json";
    {
        std::ofstream f(tmp);
        f << R"({"bind_address": "0.0.0.0:4200"})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.allowed_client_keys.empty());
    REQUIRE(cfg.allowed_peer_keys.empty());

    std::filesystem::remove(tmp);
}

TEST_CASE("load_config with empty key arrays results in empty vectors", "[config][acl]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_empty_acl.json";
    {
        std::ofstream f(tmp);
        f << R"({"allowed_client_keys": [], "allowed_peer_keys": []})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.allowed_client_keys.empty());
    REQUIRE(cfg.allowed_peer_keys.empty());

    std::filesystem::remove(tmp);
}

TEST_CASE("load_config throws on peer key with wrong length", "[config][acl]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_bad_len.json";

    SECTION("too short (63 chars)") {
        {
            std::ofstream f(tmp);
            f << R"({"allowed_peer_keys": ["a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b"]})";
        }
        REQUIRE_THROWS_AS(load_config(tmp), std::runtime_error);
    }

    SECTION("too long (65 chars)") {
        {
            std::ofstream f(tmp);
            f << R"({"allowed_peer_keys": ["a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2a"]})";
        }
        REQUIRE_THROWS_AS(load_config(tmp), std::runtime_error);
    }

    std::filesystem::remove(tmp);
}

TEST_CASE("load_config throws on client key with non-hex characters", "[config][acl]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_bad_hex.json";
    {
        std::ofstream f(tmp);
        // 'g' is not a hex char
        f << R"({"allowed_client_keys": ["g1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2"]})";
    }

    REQUIRE_THROWS_AS(load_config(tmp), std::runtime_error);

    std::filesystem::remove(tmp);
}

TEST_CASE("load_config accepts uppercase hex in allowed_peer_keys", "[config][acl]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_upper.json";
    {
        std::ofstream f(tmp);
        f << R"({"allowed_peer_keys": ["A1B2C3D4E5F6A1B2C3D4E5F6A1B2C3D4E5F6A1B2C3D4E5F6A1B2C3D4E5F6A1B2"]})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.allowed_peer_keys.size() == 1);

    std::filesystem::remove(tmp);
}

TEST_CASE("old allowed_keys field is treated as unknown key", "[config][acl]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_old_key.json";
    {
        std::ofstream f(tmp);
        f << R"({"allowed_keys": ["a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2"]})";
    }

    // Should load without error (unknown keys are warned, not rejected)
    auto cfg = load_config(tmp);
    // The old field is not parsed into any config field
    REQUIRE(cfg.allowed_client_keys.empty());
    REQUIRE(cfg.allowed_peer_keys.empty());

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

// =============================================================================
// Sync resumption: cursor config field tests
// =============================================================================

TEST_CASE("Default config has full_resync_interval 10", "[config][cursor]") {
    Config cfg;
    REQUIRE(cfg.full_resync_interval == 10);
}

TEST_CASE("Default config has cursor_stale_seconds 3600", "[config][cursor]") {
    Config cfg;
    REQUIRE(cfg.cursor_stale_seconds == 3600);
}

TEST_CASE("Config loads full_resync_interval from JSON", "[config][cursor]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_resync.json";
    {
        std::ofstream f(tmp);
        f << R"({"full_resync_interval": 25})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.full_resync_interval == 25);

    std::filesystem::remove(tmp);
}

TEST_CASE("Config loads cursor_stale_seconds from JSON", "[config][cursor]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_stale.json";
    {
        std::ofstream f(tmp);
        f << R"({"cursor_stale_seconds": 7200})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.cursor_stale_seconds == 7200);

    std::filesystem::remove(tmp);
}

TEST_CASE("Config omitting cursor fields uses defaults", "[config][cursor]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_cursor_defaults.json";
    {
        std::ofstream f(tmp);
        f << R"({"bind_address": "0.0.0.0:4200"})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.full_resync_interval == 10);
    REQUIRE(cfg.cursor_stale_seconds == 3600);

    std::filesystem::remove(tmp);
}

// =============================================================================
// Phase 35: Namespace quota config tests
// =============================================================================

TEST_CASE("quota config defaults are zero (unlimited)", "[config][quota]") {
    Config cfg;
    REQUIRE(cfg.namespace_quota_bytes == 0);
    REQUIRE(cfg.namespace_quota_count == 0);
    REQUIRE(cfg.namespace_quotas.empty());
}

TEST_CASE("quota config parsed from JSON", "[config][quota]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_quota.json";
    {
        std::ofstream f(tmp);
        f << R"({"namespace_quota_bytes": 104857600, "namespace_quota_count": 1000})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.namespace_quota_bytes == 104857600);   // 100 MiB
    REQUIRE(cfg.namespace_quota_count == 1000);

    std::filesystem::remove(tmp);
}

TEST_CASE("namespace_quotas map parsed with per-namespace overrides", "[config][quota]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_nsquota.json";
    {
        std::ofstream f(tmp);
        f << R"({
            "namespace_quota_bytes": 1000000,
            "namespace_quota_count": 500,
            "namespace_quotas": {
                "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2": {
                    "max_bytes": 5000000,
                    "max_count": 2000
                }
            }
        })";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.namespace_quota_bytes == 1000000);
    REQUIRE(cfg.namespace_quota_count == 500);
    REQUIRE(cfg.namespace_quotas.size() == 1);
    auto it = cfg.namespace_quotas.find(
        "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2");
    REQUIRE(it != cfg.namespace_quotas.end());
    REQUIRE(it->second.first.has_value());
    REQUIRE(it->second.first.value() == 5000000);
    REQUIRE(it->second.second.has_value());
    REQUIRE(it->second.second.value() == 2000);

    std::filesystem::remove(tmp);
}

TEST_CASE("namespace_quotas rejects non-64-char hex key", "[config][quota]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_badnsquota.json";

    SECTION("too short key") {
        {
            std::ofstream f(tmp);
            f << R"({"namespace_quotas": {"abcd": {"max_bytes": 1000}}})";
        }
        REQUIRE_THROWS_AS(load_config(tmp), std::runtime_error);
    }

    SECTION("non-hex key") {
        {
            std::ofstream f(tmp);
            f << R"({"namespace_quotas": {"zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz": {"max_bytes": 1000}}})";
        }
        REQUIRE_THROWS_AS(load_config(tmp), std::runtime_error);
    }

    std::filesystem::remove(tmp);
}

TEST_CASE("namespace_quotas partial override (max_bytes only)", "[config][quota]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_partial_quota.json";
    {
        std::ofstream f(tmp);
        f << R"({
            "namespace_quotas": {
                "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef": {
                    "max_bytes": 9999999
                }
            }
        })";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.namespace_quotas.size() == 1);
    auto it = cfg.namespace_quotas.find(
        "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef");
    REQUIRE(it != cfg.namespace_quotas.end());
    REQUIRE(it->second.first.has_value());
    REQUIRE(it->second.first.value() == 9999999);
    REQUIRE_FALSE(it->second.second.has_value());  // max_count not set

    std::filesystem::remove(tmp);
}

TEST_CASE("namespace_quotas override with 0 means exempt", "[config][quota]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_exempt_quota.json";
    {
        std::ofstream f(tmp);
        f << R"({
            "namespace_quota_bytes": 1000000,
            "namespace_quotas": {
                "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2": {
                    "max_bytes": 0,
                    "max_count": 0
                }
            }
        })";
    }

    auto cfg = load_config(tmp);
    auto it = cfg.namespace_quotas.find(
        "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2");
    REQUIRE(it != cfg.namespace_quotas.end());
    // 0 is explicitly set, so has_value() == true and value() == 0
    REQUIRE(it->second.first.has_value());
    REQUIRE(it->second.first.value() == 0);
    REQUIRE(it->second.second.has_value());
    REQUIRE(it->second.second.value() == 0);

    std::filesystem::remove(tmp);
}

TEST_CASE("missing quota fields use defaults", "[config][quota]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_no_quota.json";
    {
        std::ofstream f(tmp);
        f << R"({"bind_address": "0.0.0.0:4200"})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.namespace_quota_bytes == 0);
    REQUIRE(cfg.namespace_quota_count == 0);
    REQUIRE(cfg.namespace_quotas.empty());

    std::filesystem::remove(tmp);
}

// =============================================================================
// Phase 38: worker_threads config tests
// =============================================================================

TEST_CASE("Default config has worker_threads 0 (auto-detect)", "[config][threadpool]") {
    Config cfg;
    REQUIRE(cfg.worker_threads == 0);
}

TEST_CASE("worker_threads parses from JSON", "[config][threadpool]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_workers.json";
    {
        std::ofstream f(tmp);
        f << R"({"worker_threads": 4})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.worker_threads == 4);

    std::filesystem::remove(tmp);
}

TEST_CASE("worker_threads defaults to 0 when missing from JSON", "[config][threadpool]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_no_workers.json";
    {
        std::ofstream f(tmp);
        f << R"({"bind_address": "0.0.0.0:4200"})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.worker_threads == 0);

    std::filesystem::remove(tmp);
}

TEST_CASE("worker_threads 0 in JSON means auto-detect", "[config][threadpool]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_zero_workers.json";
    {
        std::ofstream f(tmp);
        f << R"({"worker_threads": 0})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.worker_threads == 0);

    std::filesystem::remove(tmp);
}

// =============================================================================
// Phase 40: Sync rate limiting config tests
// =============================================================================

TEST_CASE("Config loads sync_cooldown_seconds from JSON", "[config][syncratelimit]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_sync_cooldown.json";
    {
        std::ofstream f(tmp);
        f << R"({"sync_cooldown_seconds": 10})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.sync_cooldown_seconds == 10);

    std::filesystem::remove(tmp);
}

TEST_CASE("Config defaults sync_cooldown_seconds=30 and max_sync_sessions=1", "[config][syncratelimit]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_sync_defaults.json";
    {
        std::ofstream f(tmp);
        f << R"({})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.sync_cooldown_seconds == 30);
    REQUIRE(cfg.max_sync_sessions == 1);

    std::filesystem::remove(tmp);
}

TEST_CASE("Config loads max_sync_sessions from JSON", "[config][syncratelimit]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_max_sync.json";
    {
        std::ofstream f(tmp);
        f << R"({"max_sync_sessions": 3})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.max_sync_sessions == 3);

    std::filesystem::remove(tmp);
}

TEST_CASE("Config sync rate limit fields default when missing from JSON", "[config][syncratelimit]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_no_sync_ratelimit.json";
    {
        std::ofstream f(tmp);
        f << R"({"bind_address": "0.0.0.0:4200"})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.sync_cooldown_seconds == 30);
    REQUIRE(cfg.max_sync_sessions == 1);

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

// =============================================================================
// Phase 42: Config validation tests (validate_config)
// =============================================================================

using Catch::Matchers::ContainsSubstring;

TEST_CASE("validate_config: default Config passes", "[config][validation]") {
    Config cfg;
    REQUIRE_NOTHROW(validate_config(cfg));
}

TEST_CASE("validate_config: max_peers=0 throws", "[config][validation]") {
    Config cfg;
    cfg.max_peers = 0;
    REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
    try { validate_config(cfg); } catch (const std::runtime_error& e) {
        REQUIRE_THAT(e.what(), ContainsSubstring("max_peers"));
    }
}

TEST_CASE("validate_config: safety_net_interval_seconds below minimum throws", "[config][validation]") {
    Config cfg;

    SECTION("zero throws") {
        cfg.safety_net_interval_seconds = 0;
        REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
        try { validate_config(cfg); } catch (const std::runtime_error& e) {
            REQUIRE_THAT(e.what(), ContainsSubstring("safety_net_interval_seconds"));
        }
    }

    SECTION("2 throws") {
        cfg.safety_net_interval_seconds = 2;
        REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
        try { validate_config(cfg); } catch (const std::runtime_error& e) {
            REQUIRE_THAT(e.what(), ContainsSubstring("safety_net_interval_seconds"));
        }
    }

    SECTION("3 passes (minimum)") {
        cfg.safety_net_interval_seconds = 3;
        REQUIRE_NOTHROW(validate_config(cfg));
    }
}

TEST_CASE("validate_config: max_storage_bytes non-zero below 1 MiB throws", "[config][validation]") {
    Config cfg;
    cfg.max_storage_bytes = 100;
    REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
    try { validate_config(cfg); } catch (const std::runtime_error& e) {
        REQUIRE_THAT(e.what(), ContainsSubstring("max_storage_bytes"));
    }
}

TEST_CASE("validate_config: max_storage_bytes=0 passes (unlimited)", "[config][validation]") {
    Config cfg;
    cfg.max_storage_bytes = 0;
    REQUIRE_NOTHROW(validate_config(cfg));
}

TEST_CASE("validate_config: rate_limit_bytes_per_sec non-zero below 1024 throws", "[config][validation]") {
    Config cfg;
    cfg.rate_limit_bytes_per_sec = 500;
    REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
}

TEST_CASE("validate_config: rate_limit_burst < rate_limit_bytes_per_sec throws", "[config][validation]") {
    Config cfg;
    cfg.rate_limit_bytes_per_sec = 2048;
    cfg.rate_limit_burst = 1024;  // burst < rate
    REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
}

TEST_CASE("validate_config: rate_limit disabled (0) passes", "[config][validation]") {
    Config cfg;
    cfg.rate_limit_bytes_per_sec = 0;
    cfg.rate_limit_burst = 0;
    REQUIRE_NOTHROW(validate_config(cfg));
}

TEST_CASE("validate_config: full_resync_interval=0 throws", "[config][validation]") {
    Config cfg;
    cfg.full_resync_interval = 0;
    REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
}

TEST_CASE("validate_config: cursor_stale_seconds=30 (below 60) throws", "[config][validation]") {
    Config cfg;
    cfg.cursor_stale_seconds = 30;
    REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
}

TEST_CASE("validate_config: worker_threads=300 (above 256) throws", "[config][validation]") {
    Config cfg;
    cfg.worker_threads = 300;
    REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
}

TEST_CASE("validate_config: worker_threads=0 passes (auto-detect)", "[config][validation]") {
    Config cfg;
    cfg.worker_threads = 0;
    REQUIRE_NOTHROW(validate_config(cfg));
}

TEST_CASE("validate_config: max_sync_sessions=0 throws", "[config][validation]") {
    Config cfg;
    cfg.max_sync_sessions = 0;
    REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
}

TEST_CASE("validate_config: invalid log_level throws", "[config][validation]") {
    Config cfg;
    cfg.log_level = "verbose";
    REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
    try { validate_config(cfg); } catch (const std::runtime_error& e) {
        REQUIRE_THAT(e.what(), ContainsSubstring("log_level"));
    }
}

TEST_CASE("validate_config: valid log_levels pass", "[config][validation]") {
    for (const auto& level : {"trace", "debug", "info", "warn", "warning", "error", "err", "critical"}) {
        Config cfg;
        cfg.log_level = level;
        REQUIRE_NOTHROW(validate_config(cfg));
    }
}

TEST_CASE("validate_config: bind_address missing colon throws", "[config][validation]") {
    Config cfg;
    cfg.bind_address = "0.0.0.0";
    REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
}

TEST_CASE("validate_config: bind_address port=0 throws", "[config][validation]") {
    Config cfg;
    cfg.bind_address = "0.0.0.0:0";
    REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
}

TEST_CASE("validate_config: bind_address port=70000 throws", "[config][validation]") {
    Config cfg;
    cfg.bind_address = "0.0.0.0:70000";
    REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
}

TEST_CASE("validate_config: multiple errors accumulates all", "[config][validation]") {
    Config cfg;
    cfg.max_peers = 0;
    cfg.safety_net_interval_seconds = 0;
    cfg.log_level = "verbose";
    REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
    try { validate_config(cfg); } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        REQUIRE_THAT(msg, ContainsSubstring("max_peers"));
        REQUIRE_THAT(msg, ContainsSubstring("safety_net_interval_seconds"));
        REQUIRE_THAT(msg, ContainsSubstring("log_level"));
    }
}

TEST_CASE("load_config: type mismatch throws readable error", "[config][validation]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_type_mismatch.json";
    {
        std::ofstream f(tmp);
        f << R"({"max_peers": "thirty"})";
    }

    REQUIRE_THROWS_AS(load_config(tmp), std::runtime_error);
    try { load_config(tmp); } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        // Should be human-readable, not a raw nlohmann exception
        REQUIRE_THAT(msg, ContainsSubstring("type"));
    }

    std::filesystem::remove(tmp);
}

// =============================================================================
// Phase 43: Logging config field tests
// =============================================================================

TEST_CASE("Config defaults: log fields", "[config][logging]") {
    Config cfg;
    REQUIRE(cfg.log_file == "");
    REQUIRE(cfg.log_max_size_mb == 10);
    REQUIRE(cfg.log_max_files == 3);
    REQUIRE(cfg.log_format == "text");
}

TEST_CASE("validate_config: log_format text passes", "[config][logging]") {
    Config cfg;
    cfg.log_format = "text";
    REQUIRE_NOTHROW(validate_config(cfg));
}

TEST_CASE("validate_config: log_format json passes", "[config][logging]") {
    Config cfg;
    cfg.log_format = "json";
    REQUIRE_NOTHROW(validate_config(cfg));
}

TEST_CASE("validate_config: log_format yaml throws", "[config][logging]") {
    Config cfg;
    cfg.log_format = "yaml";
    REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
    try { validate_config(cfg); } catch (const std::runtime_error& e) {
        REQUIRE_THAT(e.what(), ContainsSubstring("log_format"));
    }
}

TEST_CASE("validate_config: log_max_size_mb=0 throws", "[config][logging]") {
    Config cfg;
    cfg.log_max_size_mb = 0;
    REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
    try { validate_config(cfg); } catch (const std::runtime_error& e) {
        REQUIRE_THAT(e.what(), ContainsSubstring("log_max_size_mb"));
    }
}

TEST_CASE("validate_config: log_max_size_mb=10 passes", "[config][logging]") {
    Config cfg;
    cfg.log_max_size_mb = 10;
    REQUIRE_NOTHROW(validate_config(cfg));
}

TEST_CASE("validate_config: log_max_files=0 throws", "[config][logging]") {
    Config cfg;
    cfg.log_max_files = 0;
    REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
    try { validate_config(cfg); } catch (const std::runtime_error& e) {
        REQUIRE_THAT(e.what(), ContainsSubstring("log_max_files"));
    }
}

TEST_CASE("validate_config: log_max_files=3 passes", "[config][logging]") {
    Config cfg;
    cfg.log_max_files = 3;
    REQUIRE_NOTHROW(validate_config(cfg));
}

TEST_CASE("validate_config: log_file empty passes", "[config][logging]") {
    Config cfg;
    cfg.log_file = "";
    REQUIRE_NOTHROW(validate_config(cfg));
}

TEST_CASE("validate_config: log_file non-empty passes", "[config][logging]") {
    Config cfg;
    cfg.log_file = "/var/log/chromatindb.log";
    REQUIRE_NOTHROW(validate_config(cfg));
}

TEST_CASE("load_config parses log config fields from JSON", "[config][logging]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_logcfg.json";
    {
        std::ofstream f(tmp);
        f << R"({
            "log_file": "/tmp/chromatindb.log",
            "log_max_size_mb": 50,
            "log_max_files": 5,
            "log_format": "json"
        })";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.log_file == "/tmp/chromatindb.log");
    REQUIRE(cfg.log_max_size_mb == 50);
    REQUIRE(cfg.log_max_files == 5);
    REQUIRE(cfg.log_format == "json");

    std::filesystem::remove(tmp);
}

TEST_CASE("load_config: missing log fields use defaults", "[config][logging]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_no_logcfg.json";
    {
        std::ofstream f(tmp);
        f << R"({"bind_address": "0.0.0.0:4200"})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.log_file == "");
    REQUIRE(cfg.log_max_size_mb == 10);
    REQUIRE(cfg.log_max_files == 3);
    REQUIRE(cfg.log_format == "text");

    std::filesystem::remove(tmp);
}

// =============================================================================
// Phase 55: Compaction interval config tests (COMP-01)
// =============================================================================

TEST_CASE("Config defaults: compaction_interval_hours is 6", "[config][compaction]") {
    Config cfg;
    REQUIRE(cfg.compaction_interval_hours == 6);
}

TEST_CASE("validate_config: compaction_interval_hours=0 passes (disabled)", "[config][compaction]") {
    Config cfg;
    cfg.compaction_interval_hours = 0;
    REQUIRE_NOTHROW(validate_config(cfg));
}

TEST_CASE("validate_config: compaction_interval_hours=1 passes (minimum when enabled)", "[config][compaction]") {
    Config cfg;
    cfg.compaction_interval_hours = 1;
    REQUIRE_NOTHROW(validate_config(cfg));
}

TEST_CASE("validate_config: compaction_interval_hours=6 passes (default)", "[config][compaction]") {
    Config cfg;
    cfg.compaction_interval_hours = 6;
    REQUIRE_NOTHROW(validate_config(cfg));
}

TEST_CASE("validate_config: compaction_interval_hours=168 passes (1 week)", "[config][compaction]") {
    Config cfg;
    cfg.compaction_interval_hours = 168;
    REQUIRE_NOTHROW(validate_config(cfg));
}

TEST_CASE("load_config reads compaction_interval_hours from JSON", "[config][compaction]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_compaction.json";
    {
        std::ofstream f(tmp);
        f << R"({"compaction_interval_hours": 12})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.compaction_interval_hours == 12);

    std::filesystem::remove(tmp);
}

TEST_CASE("load_config: missing compaction_interval_hours uses default 6", "[config][compaction]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_no_compaction.json";
    {
        std::ofstream f(tmp);
        f << R"({"bind_address": "0.0.0.0:4200"})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.compaction_interval_hours == 6);

    std::filesystem::remove(tmp);
}

TEST_CASE("load_config: compaction_interval_hours=0 disables compaction", "[config][compaction]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_compaction_off.json";
    {
        std::ofstream f(tmp);
        f << R"({"compaction_interval_hours": 0})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.compaction_interval_hours == 0);

    std::filesystem::remove(tmp);
}

TEST_CASE("compaction_interval_hours is a known config key", "[config][compaction]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_compaction_known.json";
    {
        std::ofstream f(tmp);
        f << R"({"compaction_interval_hours": 6})";
    }

    // Should not produce "unknown config key" warning -- verified by no throw
    REQUIRE_NOTHROW(load_config(tmp));

    std::filesystem::remove(tmp);
}

// =============================================================================
// Phase 54: Sync rejection reason string tests (OPS-03)
// =============================================================================

#include "db/peer/sync_reject.h"

TEST_CASE("sync_reject_reason_string: all 8 reason codes", "[syncreason]") {
    using namespace chromatindb::peer;

    REQUIRE(sync_reject_reason_string(SYNC_REJECT_COOLDOWN) == "cooldown");
    REQUIRE(sync_reject_reason_string(SYNC_REJECT_SESSION_LIMIT) == "session_limit");
    REQUIRE(sync_reject_reason_string(SYNC_REJECT_BYTE_RATE) == "byte_rate");
    REQUIRE(sync_reject_reason_string(SYNC_REJECT_STORAGE_FULL) == "storage_full");
    REQUIRE(sync_reject_reason_string(SYNC_REJECT_QUOTA_EXCEEDED) == "quota_exceeded");
    REQUIRE(sync_reject_reason_string(SYNC_REJECT_NAMESPACE_NOT_FOUND) == "namespace_not_found");
    REQUIRE(sync_reject_reason_string(SYNC_REJECT_BLOB_TOO_LARGE) == "blob_too_large");
    REQUIRE(sync_reject_reason_string(SYNC_REJECT_TIMESTAMP_REJECTED) == "timestamp_rejected");
}

TEST_CASE("sync_reject_reason_string: unknown byte returns unknown", "[syncreason]") {
    using namespace chromatindb::peer;

    REQUIRE(sync_reject_reason_string(0x00) == "unknown");
    REQUIRE(sync_reject_reason_string(0x09) == "unknown");
    REQUIRE(sync_reject_reason_string(0xFF) == "unknown");
}

TEST_CASE("load_config: unknown keys do not throw", "[config][validation]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_unknown_keys.json";
    {
        std::ofstream f(tmp);
        f << R"({"bind_address": "0.0.0.0:4200", "unknown_future_field": 42, "another_mystery": true})";
    }

    // Should not throw -- unknown keys are warned, not rejected
    REQUIRE_NOTHROW(load_config(tmp));

    std::filesystem::remove(tmp);
}

// =============================================================================
// Phase 56: UDS path config tests (UDS-01)
// =============================================================================

TEST_CASE("uds_path defaults to empty", "[config][uds]") {
    Config cfg;
    REQUIRE(cfg.uds_path.empty());
}

TEST_CASE("load_config parses uds_path", "[config][uds]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_uds_path.json";
    {
        std::ofstream f(tmp);
        f << R"({"uds_path": "/tmp/chromatindb.sock"})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.uds_path == "/tmp/chromatindb.sock");

    std::filesystem::remove(tmp);
}

TEST_CASE("validate_config rejects relative uds_path", "[config][uds]") {
    Config cfg;
    cfg.uds_path = "relative/path.sock";
    REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
    try { validate_config(cfg); } catch (const std::runtime_error& e) {
        REQUIRE_THAT(e.what(), ContainsSubstring("absolute path"));
    }
}

TEST_CASE("validate_config accepts absolute uds_path", "[config][uds]") {
    Config cfg;
    cfg.uds_path = "/tmp/chromatindb.sock";
    REQUIRE_NOTHROW(validate_config(cfg));
}

TEST_CASE("validate_config accepts empty uds_path", "[config][uds]") {
    Config cfg;
    cfg.uds_path = "";
    REQUIRE_NOTHROW(validate_config(cfg));
}

TEST_CASE("validate_config rejects too-long uds_path", "[config][uds]") {
    Config cfg;
    cfg.uds_path = "/" + std::string(199, 'a');  // 200 chars total, starts with /
    REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
    try { validate_config(cfg); } catch (const std::runtime_error& e) {
        REQUIRE_THAT(e.what(), ContainsSubstring("too long"));
    }
}

// =============================================================================
// Phase 90: metrics_bind config tests (OPS-02)
// =============================================================================

using Catch::Matchers::ContainsSubstring;

TEST_CASE("metrics_bind defaults to empty", "[config][metrics]") {
    Config cfg;
    REQUIRE(cfg.metrics_bind.empty());
}

TEST_CASE("load_config parses metrics_bind from JSON", "[config][metrics]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_metrics_bind.json";
    {
        std::ofstream f(tmp);
        f << R"({"metrics_bind": "127.0.0.1:9090"})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.metrics_bind == "127.0.0.1:9090");

    std::filesystem::remove(tmp);
}

TEST_CASE("load_config: missing metrics_bind uses default empty", "[config][metrics]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_metrics_bind_missing.json";
    {
        std::ofstream f(tmp);
        f << R"({"bind_address": "0.0.0.0:4200"})";
    }

    auto cfg = load_config(tmp);
    REQUIRE(cfg.metrics_bind.empty());

    std::filesystem::remove(tmp);
}

TEST_CASE("validate_config: metrics_bind empty passes", "[config][metrics]") {
    Config cfg;
    REQUIRE_NOTHROW(validate_config(cfg));
}

TEST_CASE("validate_config: metrics_bind valid host:port passes", "[config][metrics]") {
    Config cfg;
    cfg.metrics_bind = "127.0.0.1:9090";
    REQUIRE_NOTHROW(validate_config(cfg));
}

TEST_CASE("validate_config: metrics_bind missing colon throws", "[config][metrics]") {
    Config cfg;
    cfg.metrics_bind = "localhost9090";
    REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
}

TEST_CASE("validate_config: metrics_bind port=0 throws", "[config][metrics]") {
    Config cfg;
    cfg.metrics_bind = "127.0.0.1:0";
    REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
}

TEST_CASE("validate_config: metrics_bind port=70000 throws", "[config][metrics]") {
    Config cfg;
    cfg.metrics_bind = "127.0.0.1:70000";
    REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
}

TEST_CASE("metrics_bind is a known config key", "[config][metrics]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_metrics_known.json";
    {
        std::ofstream f(tmp);
        f << R"({"metrics_bind": "127.0.0.1:9090"})";
    }

    // Should not produce "unknown config key" warning -- verified by no throw
    REQUIRE_NOTHROW(load_config(tmp));

    std::filesystem::remove(tmp);
}

// ===== Phase 118: Configurable sync/peer constants =====

TEST_CASE("Phase 118 config defaults", "[config]") {
    Config cfg;
    REQUIRE(cfg.blob_transfer_timeout == 600);
    REQUIRE(cfg.sync_timeout == 30);
    REQUIRE(cfg.pex_interval == 300);
    REQUIRE(cfg.strike_threshold == 10);
    REQUIRE(cfg.strike_cooldown == 300);
}

TEST_CASE("blob_transfer_timeout parses from JSON", "[config]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_btt.json";
    {
        std::ofstream f(tmp);
        f << R"({"blob_transfer_timeout": 120})";
    }
    auto cfg = load_config(tmp);
    REQUIRE(cfg.blob_transfer_timeout == 120);
    std::filesystem::remove(tmp);
}

TEST_CASE("sync_timeout parses from JSON", "[config]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_st.json";
    {
        std::ofstream f(tmp);
        f << R"({"sync_timeout": 60})";
    }
    auto cfg = load_config(tmp);
    REQUIRE(cfg.sync_timeout == 60);
    std::filesystem::remove(tmp);
}

TEST_CASE("pex_interval parses from JSON", "[config]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_pex.json";
    {
        std::ofstream f(tmp);
        f << R"({"pex_interval": 600})";
    }
    auto cfg = load_config(tmp);
    REQUIRE(cfg.pex_interval == 600);
    std::filesystem::remove(tmp);
}

TEST_CASE("strike_threshold parses from JSON", "[config]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_strike.json";
    {
        std::ofstream f(tmp);
        f << R"({"strike_threshold": 5})";
    }
    auto cfg = load_config(tmp);
    REQUIRE(cfg.strike_threshold == 5);
    std::filesystem::remove(tmp);
}

TEST_CASE("strike_cooldown parses from JSON", "[config]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_sc.json";
    {
        std::ofstream f(tmp);
        f << R"({"strike_cooldown": 600})";
    }
    auto cfg = load_config(tmp);
    REQUIRE(cfg.strike_cooldown == 600);
    std::filesystem::remove(tmp);
}

TEST_CASE("validate_config: blob_transfer_timeout below minimum throws", "[config][validation]") {
    Config cfg;
    cfg.blob_transfer_timeout = 5;
    REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
    try { validate_config(cfg); } catch (const std::runtime_error& e) {
        REQUIRE_THAT(e.what(), Catch::Matchers::ContainsSubstring("blob_transfer_timeout"));
    }
}

TEST_CASE("validate_config: blob_transfer_timeout above maximum throws", "[config][validation]") {
    Config cfg;
    cfg.blob_transfer_timeout = 100000;
    REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
    try { validate_config(cfg); } catch (const std::runtime_error& e) {
        REQUIRE_THAT(e.what(), Catch::Matchers::ContainsSubstring("blob_transfer_timeout"));
    }
}

TEST_CASE("validate_config: sync_timeout below minimum throws", "[config][validation]") {
    Config cfg;
    cfg.sync_timeout = 2;
    REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
    try { validate_config(cfg); } catch (const std::runtime_error& e) {
        REQUIRE_THAT(e.what(), Catch::Matchers::ContainsSubstring("sync_timeout"));
    }
}

TEST_CASE("validate_config: sync_timeout above maximum throws", "[config][validation]") {
    Config cfg;
    cfg.sync_timeout = 5000;
    REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
    try { validate_config(cfg); } catch (const std::runtime_error& e) {
        REQUIRE_THAT(e.what(), Catch::Matchers::ContainsSubstring("sync_timeout"));
    }
}

TEST_CASE("validate_config: pex_interval below minimum throws", "[config][validation]") {
    Config cfg;
    cfg.pex_interval = 5;
    REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
    try { validate_config(cfg); } catch (const std::runtime_error& e) {
        REQUIRE_THAT(e.what(), Catch::Matchers::ContainsSubstring("pex_interval"));
    }
}

TEST_CASE("validate_config: pex_interval above maximum throws", "[config][validation]") {
    Config cfg;
    cfg.pex_interval = 100000;
    REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
    try { validate_config(cfg); } catch (const std::runtime_error& e) {
        REQUIRE_THAT(e.what(), Catch::Matchers::ContainsSubstring("pex_interval"));
    }
}

TEST_CASE("validate_config: strike_threshold zero throws", "[config][validation]") {
    Config cfg;
    cfg.strike_threshold = 0;
    REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
    try { validate_config(cfg); } catch (const std::runtime_error& e) {
        REQUIRE_THAT(e.what(), Catch::Matchers::ContainsSubstring("strike_threshold"));
    }
}

TEST_CASE("validate_config: strike_threshold above maximum throws", "[config][validation]") {
    Config cfg;
    cfg.strike_threshold = 1001;
    REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
    try { validate_config(cfg); } catch (const std::runtime_error& e) {
        REQUIRE_THAT(e.what(), Catch::Matchers::ContainsSubstring("strike_threshold"));
    }
}

TEST_CASE("validate_config: strike_cooldown above maximum throws", "[config][validation]") {
    Config cfg;
    cfg.strike_cooldown = 100000;
    REQUIRE_THROWS_AS(validate_config(cfg), std::runtime_error);
    try { validate_config(cfg); } catch (const std::runtime_error& e) {
        REQUIRE_THAT(e.what(), Catch::Matchers::ContainsSubstring("strike_cooldown"));
    }
}

TEST_CASE("validate_config: Phase 118 defaults pass validation", "[config][validation]") {
    Config cfg;
    REQUIRE_NOTHROW(validate_config(cfg));
}

TEST_CASE("Phase 118 keys are known config keys", "[config]") {
    auto tmp = std::filesystem::temp_directory_path() / "chromatindb_test_p118_known.json";
    {
        std::ofstream f(tmp);
        f << R"({
            "blob_transfer_timeout": 600,
            "sync_timeout": 30,
            "pex_interval": 300,
            "strike_threshold": 10,
            "strike_cooldown": 300
        })";
    }

    // Should not produce "unknown config key" warnings
    REQUIRE_NOTHROW(load_config(tmp));

    std::filesystem::remove(tmp);
}
