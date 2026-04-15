#include <catch2/catch_test_macros.hpp>
#include "cli/src/identity.h"
#include <oqs/sha3.h>
#include <algorithm>
#include <filesystem>
#include <random>

namespace fs = std::filesystem;

/// RAII temp dir for tests.
struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& prefix = "cli_test_") {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint64_t> dist;
        path = fs::temp_directory_path() / (prefix + std::to_string(dist(gen)));
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

TEST_CASE("identity: generate creates signing + KEM keypairs", "[identity]") {
    auto id = chromatindb::cli::Identity::generate();

    REQUIRE(id.signing_pubkey().size() == 2592);   // ML-DSA-87
    REQUIRE(id.signing_seckey().size() == 4896);
    REQUIRE(id.kem_pubkey().size() == 1568);        // ML-KEM-1024
    REQUIRE(id.kem_seckey().size() == 3168);
    REQUIRE(id.namespace_id().size() == 32);

    // Namespace must not be all zeros
    auto ns = id.namespace_id();
    bool all_zero = std::all_of(ns.begin(), ns.end(), [](uint8_t b) { return b == 0; });
    REQUIRE_FALSE(all_zero);
}

TEST_CASE("identity: save and load roundtrip", "[identity]") {
    TempDir tmp;

    auto original = chromatindb::cli::Identity::generate();
    original.save_to(tmp.path);

    // Verify files exist
    REQUIRE(fs::exists(tmp.path / "identity.pub"));
    REQUIRE(fs::exists(tmp.path / "identity.key"));
    REQUIRE(fs::exists(tmp.path / "identity.kpub"));
    REQUIRE(fs::exists(tmp.path / "identity.kem"));

    auto loaded = chromatindb::cli::Identity::load_from(tmp.path);

    // All keys must match
    REQUIRE(std::equal(original.signing_pubkey().begin(), original.signing_pubkey().end(),
                       loaded.signing_pubkey().begin()));
    REQUIRE(std::equal(original.signing_seckey().begin(), original.signing_seckey().end(),
                       loaded.signing_seckey().begin()));
    REQUIRE(std::equal(original.kem_pubkey().begin(), original.kem_pubkey().end(),
                       loaded.kem_pubkey().begin()));
    REQUIRE(std::equal(original.kem_seckey().begin(), original.kem_seckey().end(),
                       loaded.kem_seckey().begin()));
    REQUIRE(std::equal(original.namespace_id().begin(), original.namespace_id().end(),
                       loaded.namespace_id().begin()));

    // Loaded identity can sign
    std::vector<uint8_t> msg = {1, 2, 3, 4, 5};
    auto sig = loaded.sign(msg);
    REQUIRE(!sig.empty());
}

TEST_CASE("identity: namespace is SHA3-256 of signing pubkey", "[identity]") {
    auto id = chromatindb::cli::Identity::generate();

    // Manually compute SHA3-256 of signing pubkey using liboqs
    auto pk = id.signing_pubkey();
    std::array<uint8_t, 32> expected{};
    OQS_SHA3_sha3_256(expected.data(), pk.data(), pk.size());

    auto ns = id.namespace_id();
    REQUIRE(std::equal(ns.begin(), ns.end(), expected.begin()));
}

TEST_CASE("identity: export_public_keys produces signing + KEM pubkeys", "[identity]") {
    auto id = chromatindb::cli::Identity::generate();

    auto exported = id.export_public_keys();
    REQUIRE(exported.size() == 2592 + 1568);  // signing_pk + kem_pk = 4160 bytes

    // First 2592 bytes = signing pubkey
    REQUIRE(std::equal(id.signing_pubkey().begin(), id.signing_pubkey().end(),
                       exported.begin()));

    // Next 1568 bytes = KEM pubkey
    REQUIRE(std::equal(id.kem_pubkey().begin(), id.kem_pubkey().end(),
                       exported.begin() + 2592));
}

TEST_CASE("identity: load_public_keys roundtrips with export", "[identity]") {
    auto id = chromatindb::cli::Identity::generate();

    auto exported = id.export_public_keys();
    auto [signing_pk, kem_pk] = chromatindb::cli::Identity::load_public_keys(exported);

    REQUIRE(signing_pk.size() == 2592);
    REQUIRE(kem_pk.size() == 1568);
    REQUIRE(std::equal(id.signing_pubkey().begin(), id.signing_pubkey().end(),
                       signing_pk.begin()));
    REQUIRE(std::equal(id.kem_pubkey().begin(), id.kem_pubkey().end(),
                       kem_pk.begin()));
}

TEST_CASE("identity: load_public_keys rejects wrong size", "[identity]") {
    std::vector<uint8_t> bad(100, 0);
    REQUIRE_THROWS_AS(chromatindb::cli::Identity::load_public_keys(bad), std::runtime_error);
}

TEST_CASE("identity: load_from throws on missing files", "[identity]") {
    TempDir tmp;
    fs::create_directories(tmp.path);
    REQUIRE_THROWS_AS(chromatindb::cli::Identity::load_from(tmp.path), std::runtime_error);
}

TEST_CASE("identity: two identities have different namespaces", "[identity]") {
    auto id1 = chromatindb::cli::Identity::generate();
    auto id2 = chromatindb::cli::Identity::generate();

    REQUIRE_FALSE(std::equal(id1.namespace_id().begin(), id1.namespace_id().end(),
                             id2.namespace_id().begin()));
}
