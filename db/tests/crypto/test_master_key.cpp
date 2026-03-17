#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>

#include "db/crypto/master_key.h"
#include "db/crypto/aead.h"

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;

    TempDir() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint64_t> dist;
        path = fs::temp_directory_path() /
               ("chromatindb_mk_test_" + std::to_string(dist(gen)));
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

} // anonymous namespace

TEST_CASE("Master key generates 32-byte key on first run", "[master-key]") {
    TempDir tmp;
    auto key = chromatindb::crypto::load_or_generate_master_key(tmp.path);

    REQUIRE(key.size() == 32);
    REQUIRE(fs::exists(tmp.path / "master.key"));
}

TEST_CASE("Master key loads existing key on second call", "[master-key]") {
    TempDir tmp;
    auto key1 = chromatindb::crypto::load_or_generate_master_key(tmp.path);
    auto key2 = chromatindb::crypto::load_or_generate_master_key(tmp.path);

    REQUIRE(key1.size() == key2.size());
    REQUIRE(std::memcmp(key1.data(), key2.data(), key1.size()) == 0);
}

TEST_CASE("Master key file has restricted permissions", "[master-key]") {
    TempDir tmp;
    chromatindb::crypto::load_or_generate_master_key(tmp.path);

    auto key_path = tmp.path / "master.key";
    auto perms = fs::status(key_path).permissions();

    // Only owner_read and owner_write should be set (0600)
    REQUIRE((perms & fs::perms::owner_read) != fs::perms::none);
    REQUIRE((perms & fs::perms::owner_write) != fs::perms::none);
    REQUIRE((perms & fs::perms::group_read) == fs::perms::none);
    REQUIRE((perms & fs::perms::group_write) == fs::perms::none);
    REQUIRE((perms & fs::perms::others_read) == fs::perms::none);
    REQUIRE((perms & fs::perms::others_write) == fs::perms::none);
}

TEST_CASE("Master key rejects wrong size file", "[master-key]") {
    TempDir tmp;
    fs::create_directories(tmp.path);

    // Write a 16-byte file (wrong size)
    auto key_path = tmp.path / "master.key";
    {
        std::ofstream f(key_path, std::ios::binary);
        std::vector<uint8_t> bad_key(16, 0xAB);
        f.write(reinterpret_cast<const char*>(bad_key.data()), bad_key.size());
    }

    REQUIRE_THROWS_AS(
        chromatindb::crypto::load_or_generate_master_key(tmp.path),
        std::runtime_error);
}

TEST_CASE("derive_blob_key produces correct size", "[master-key]") {
    TempDir tmp;
    auto master_key = chromatindb::crypto::load_or_generate_master_key(tmp.path);
    auto blob_key = chromatindb::crypto::derive_blob_key(master_key);

    REQUIRE(blob_key.size() == chromatindb::crypto::AEAD::KEY_SIZE);
}

TEST_CASE("derive_blob_key is deterministic", "[master-key]") {
    TempDir tmp;
    auto master_key = chromatindb::crypto::load_or_generate_master_key(tmp.path);
    auto key1 = chromatindb::crypto::derive_blob_key(master_key);
    auto key2 = chromatindb::crypto::derive_blob_key(master_key);

    REQUIRE(key1.size() == key2.size());
    REQUIRE(std::memcmp(key1.data(), key2.data(), key1.size()) == 0);
}

TEST_CASE("derive_blob_key differs for different master keys", "[master-key]") {
    TempDir tmp1, tmp2;
    auto mk1 = chromatindb::crypto::load_or_generate_master_key(tmp1.path);
    auto mk2 = chromatindb::crypto::load_or_generate_master_key(tmp2.path);
    auto bk1 = chromatindb::crypto::derive_blob_key(mk1);
    auto bk2 = chromatindb::crypto::derive_blob_key(mk2);

    REQUIRE(bk1.size() == bk2.size());
    REQUIRE(std::memcmp(bk1.data(), bk2.data(), bk1.size()) != 0);
}
