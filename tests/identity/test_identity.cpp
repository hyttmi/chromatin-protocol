#include <catch2/catch_test_macros.hpp>
#include "identity/identity.h"
#include "crypto/hash.h"
#include "crypto/signing.h"
#include <filesystem>
#include <fstream>

using namespace chromatin::identity;

TEST_CASE("NodeIdentity::generate creates valid identity", "[identity]") {
    auto id = NodeIdentity::generate();

    // Namespace should be 32 bytes (SHA3-256)
    auto ns = id.namespace_id();
    REQUIRE(ns.size() == 32);

    // Public key should be ML-DSA-87 size
    auto pk = id.public_key();
    REQUIRE(pk.size() == chromatin::crypto::Signer::PUBLIC_KEY_SIZE);

    // Namespace should not be all zeros
    bool all_zero = true;
    for (auto b : ns) {
        if (b != 0) { all_zero = false; break; }
    }
    REQUIRE_FALSE(all_zero);
}

TEST_CASE("Namespace derivation is deterministic: SHA3-256(pubkey)", "[identity]") {
    auto id = NodeIdentity::generate();

    // Manually derive namespace from public key
    auto pk = id.public_key();
    auto expected_ns = chromatin::crypto::sha3_256(pk);

    auto actual_ns = id.namespace_id();
    REQUIRE(std::equal(actual_ns.begin(), actual_ns.end(), expected_ns.begin()));
}

TEST_CASE("Two generated identities have different namespaces", "[identity]") {
    auto id1 = NodeIdentity::generate();
    auto id2 = NodeIdentity::generate();

    auto ns1 = id1.namespace_id();
    auto ns2 = id2.namespace_id();

    REQUIRE_FALSE(std::equal(ns1.begin(), ns1.end(), ns2.begin()));
}

TEST_CASE("NodeIdentity can sign messages", "[identity]") {
    auto id = NodeIdentity::generate();

    std::vector<uint8_t> msg = {1, 2, 3, 4, 5};
    auto sig = id.sign(msg);

    REQUIRE(sig.size() == chromatin::crypto::Signer::MAX_SIGNATURE_SIZE);

    // Verify signature with public key
    REQUIRE(chromatin::crypto::Signer::verify(msg, sig, id.public_key()));
}

TEST_CASE("save_to and load_from round-trip correctly", "[identity]") {
    auto tmp_dir = std::filesystem::temp_directory_path() / "chromatindb_test_identity";
    std::filesystem::create_directories(tmp_dir);

    // Generate and save
    auto original = NodeIdentity::generate();
    original.save_to(tmp_dir);

    // Verify files exist with correct sizes
    REQUIRE(std::filesystem::exists(tmp_dir / "node.pub"));
    REQUIRE(std::filesystem::exists(tmp_dir / "node.key"));
    REQUIRE(std::filesystem::file_size(tmp_dir / "node.pub") == chromatin::crypto::Signer::PUBLIC_KEY_SIZE);
    REQUIRE(std::filesystem::file_size(tmp_dir / "node.key") == chromatin::crypto::Signer::SECRET_KEY_SIZE);

    // Load and compare
    auto loaded = NodeIdentity::load_from(tmp_dir);

    auto orig_ns = original.namespace_id();
    auto load_ns = loaded.namespace_id();
    REQUIRE(std::equal(orig_ns.begin(), orig_ns.end(), load_ns.begin()));

    auto orig_pk = original.public_key();
    auto load_pk = loaded.public_key();
    REQUIRE(std::equal(orig_pk.begin(), orig_pk.end(), load_pk.begin()));

    // Loaded identity can sign and original can verify
    std::vector<uint8_t> msg = {10, 20, 30};
    auto sig = loaded.sign(msg);
    REQUIRE(chromatin::crypto::Signer::verify(msg, sig, original.public_key()));

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("load_from throws on missing files", "[identity]") {
    auto tmp_dir = std::filesystem::temp_directory_path() / "chromatindb_test_identity_missing";
    std::filesystem::create_directories(tmp_dir);

    REQUIRE_THROWS_AS(NodeIdentity::load_from(tmp_dir), std::runtime_error);

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("load_from throws on corrupt key file", "[identity]") {
    auto tmp_dir = std::filesystem::temp_directory_path() / "chromatindb_test_identity_corrupt";
    std::filesystem::create_directories(tmp_dir);

    // Write a valid-size pub file but wrong-size key file
    {
        std::ofstream pub(tmp_dir / "node.pub", std::ios::binary);
        std::vector<uint8_t> fake_pub(chromatin::crypto::Signer::PUBLIC_KEY_SIZE, 0);
        pub.write(reinterpret_cast<const char*>(fake_pub.data()), fake_pub.size());
    }
    {
        std::ofstream key(tmp_dir / "node.key", std::ios::binary);
        std::vector<uint8_t> fake_key(10, 0);  // Wrong size
        key.write(reinterpret_cast<const char*>(fake_key.data()), fake_key.size());
    }

    REQUIRE_THROWS_AS(NodeIdentity::load_from(tmp_dir), std::runtime_error);

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("load_or_generate generates and saves when no files exist", "[identity]") {
    auto tmp_dir = std::filesystem::temp_directory_path() / "chromatindb_test_identity_autogen";
    // Ensure clean slate
    std::filesystem::remove_all(tmp_dir);

    auto id = NodeIdentity::load_or_generate(tmp_dir);

    // Files should now exist
    REQUIRE(std::filesystem::exists(tmp_dir / "node.pub"));
    REQUIRE(std::filesystem::exists(tmp_dir / "node.key"));

    // Namespace should be valid
    auto ns = id.namespace_id();
    REQUIRE(ns.size() == 32);

    // Loading again should produce the same identity
    auto id2 = NodeIdentity::load_or_generate(tmp_dir);
    auto ns2 = id2.namespace_id();
    REQUIRE(std::equal(ns.begin(), ns.end(), ns2.begin()));

    std::filesystem::remove_all(tmp_dir);
}
