#include <catch2/catch_test_macros.hpp>
#include "relay/identity/relay_identity.h"
#include "db/crypto/signing.h"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "db/tests/test_helpers.h"

namespace fs = std::filesystem;
using chromatindb::relay::identity::RelayIdentity;
using chromatindb::relay::identity::pub_path_from_key_path;

namespace {

using chromatindb::test::TempDir;

/// Write arbitrary data to file for corruption tests.
void write_file(const fs::path& p, const std::vector<uint8_t>& data) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
}

} // namespace

TEST_CASE("pub_path_from_key_path replaces .key with .pub", "[relay_identity]") {
    REQUIRE(pub_path_from_key_path("/etc/chromatindb/relay.key") == "/etc/chromatindb/relay.pub");
    REQUIRE(pub_path_from_key_path("/tmp/test_relay.key") == "/tmp/test_relay.pub");
    REQUIRE(pub_path_from_key_path("relay.key") == "relay.pub");
}

TEST_CASE("RelayIdentity::generate creates valid identity", "[relay_identity]") {
    auto id = RelayIdentity::generate();
    REQUIRE(id.public_key().size() == chromatindb::crypto::Signer::PUBLIC_KEY_SIZE);
}

TEST_CASE("RelayIdentity::generate produces 32-byte public key hash", "[relay_identity]") {
    auto id = RelayIdentity::generate();
    auto hash = id.public_key_hash();
    REQUIRE(hash.size() == 32);

    // Hash should not be all zeros (generated identity has real key)
    bool all_zero = true;
    for (auto b : hash) {
        if (b != 0) { all_zero = false; break; }
    }
    REQUIRE_FALSE(all_zero);
}

TEST_CASE("RelayIdentity::save_to creates .key and .pub files", "[relay_identity]") {
    TempDir tmp;
    fs::create_directories(tmp.path);
    auto key_path = tmp.path / "relay.key";
    auto pub_path = tmp.path / "relay.pub";

    auto id = RelayIdentity::generate();
    id.save_to(key_path);

    REQUIRE(fs::exists(key_path));
    REQUIRE(fs::exists(pub_path));
    REQUIRE(fs::file_size(key_path) == chromatindb::crypto::Signer::SECRET_KEY_SIZE);
    REQUIRE(fs::file_size(pub_path) == chromatindb::crypto::Signer::PUBLIC_KEY_SIZE);
}

TEST_CASE("save_to sibling path: /tmp/test_relay.key creates /tmp/test_relay.pub", "[relay_identity]") {
    TempDir tmp;
    fs::create_directories(tmp.path);
    auto key_path = tmp.path / "test_relay.key";

    auto id = RelayIdentity::generate();
    id.save_to(key_path);

    // Should create test_relay.pub, NOT test_relay.key.pub
    auto expected_pub = tmp.path / "test_relay.pub";
    REQUIRE(fs::exists(expected_pub));
    REQUIRE_FALSE(fs::exists(tmp.path / "test_relay.key.pub"));
}

TEST_CASE("RelayIdentity::load_from loads identity from .key + .pub files", "[relay_identity]") {
    TempDir tmp;
    fs::create_directories(tmp.path);
    auto key_path = tmp.path / "relay.key";

    auto original = RelayIdentity::generate();
    original.save_to(key_path);

    auto loaded = RelayIdentity::load_from(key_path);

    // Public keys must match
    auto orig_pk = original.public_key();
    auto load_pk = loaded.public_key();
    REQUIRE(orig_pk.size() == load_pk.size());
    REQUIRE(std::equal(orig_pk.begin(), orig_pk.end(), load_pk.begin()));

    // Hashes must match
    auto orig_hash = original.public_key_hash();
    auto load_hash = loaded.public_key_hash();
    REQUIRE(std::equal(orig_hash.begin(), orig_hash.end(), load_hash.begin()));
}

TEST_CASE("load_from throws if .key file missing", "[relay_identity]") {
    TempDir tmp;
    fs::create_directories(tmp.path);
    auto key_path = tmp.path / "nonexistent.key";

    REQUIRE_THROWS_AS(RelayIdentity::load_from(key_path), std::runtime_error);
}

TEST_CASE("load_from throws if .pub file missing", "[relay_identity]") {
    TempDir tmp;
    fs::create_directories(tmp.path);
    auto key_path = tmp.path / "relay.key";

    // Create only the .key file, no .pub
    auto id = RelayIdentity::generate();
    id.save_to(key_path);
    fs::remove(tmp.path / "relay.pub");

    REQUIRE_THROWS_AS(RelayIdentity::load_from(key_path), std::runtime_error);
}

TEST_CASE("load_from throws if .key file has wrong size", "[relay_identity]") {
    TempDir tmp;
    fs::create_directories(tmp.path);
    auto key_path = tmp.path / "relay.key";
    auto pub_path = tmp.path / "relay.pub";

    // Create valid pub, corrupt key
    auto id = RelayIdentity::generate();
    id.save_to(key_path);
    write_file(key_path, std::vector<uint8_t>(100, 0xFF)); // wrong size

    REQUIRE_THROWS_AS(RelayIdentity::load_from(key_path), std::runtime_error);
}

TEST_CASE("load_or_generate creates files when none exist", "[relay_identity]") {
    TempDir tmp;
    fs::create_directories(tmp.path);
    auto key_path = tmp.path / "relay.key";
    auto pub_path = tmp.path / "relay.pub";

    REQUIRE_FALSE(fs::exists(key_path));
    REQUIRE_FALSE(fs::exists(pub_path));

    auto id = RelayIdentity::load_or_generate(key_path);

    REQUIRE(fs::exists(key_path));
    REQUIRE(fs::exists(pub_path));
    REQUIRE(id.public_key().size() == chromatindb::crypto::Signer::PUBLIC_KEY_SIZE);
}

TEST_CASE("load_or_generate loads existing without overwriting", "[relay_identity]") {
    TempDir tmp;
    fs::create_directories(tmp.path);
    auto key_path = tmp.path / "relay.key";

    // First: generate and save
    auto original = RelayIdentity::generate();
    original.save_to(key_path);

    // Record modification time
    auto key_time = fs::last_write_time(key_path);

    // Second: load_or_generate should load, not overwrite
    auto loaded = RelayIdentity::load_or_generate(key_path);

    // File time should not change
    REQUIRE(fs::last_write_time(key_path) == key_time);

    // Public keys must match
    auto orig_pk = original.public_key();
    auto load_pk = loaded.public_key();
    REQUIRE(std::equal(orig_pk.begin(), orig_pk.end(), load_pk.begin()));
}
