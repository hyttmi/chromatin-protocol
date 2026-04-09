#include <catch2/catch_test_macros.hpp>

#include "relay/core/authenticator.h"
#include "relay/identity/relay_identity.h"

using chromatindb::relay::core::Authenticator;
using chromatindb::relay::identity::RelayIdentity;

TEST_CASE("Authenticator: generate_challenge returns 32 bytes", "[auth]") {
    Authenticator auth;
    auto c1 = auth.generate_challenge();
    auto c2 = auth.generate_challenge();

    REQUIRE(c1.size() == 32);
    REQUIRE(c2.size() == 32);

    // Two challenges must differ (randomness)
    REQUIRE(c1 != c2);
}

TEST_CASE("Authenticator: verify rejects wrong pubkey size", "[auth]") {
    Authenticator auth;
    auto challenge = auth.generate_challenge();

    std::vector<uint8_t> bad_pubkey(100, 0x42);
    std::vector<uint8_t> sig(4627, 0x00);

    auto result = auth.verify(challenge, bad_pubkey, sig);
    REQUIRE(!result.success);
    REQUIRE(result.error_code == "bad_pubkey_size");
}

TEST_CASE("Authenticator: verify rejects wrong signature size", "[auth]") {
    Authenticator auth;
    auto challenge = auth.generate_challenge();

    std::vector<uint8_t> pubkey(2592, 0x00);
    std::vector<uint8_t> bad_sig(100, 0x00);

    auto result = auth.verify(challenge, pubkey, bad_sig);
    REQUIRE(!result.success);
    REQUIRE(result.error_code == "bad_signature_size");
}

TEST_CASE("Authenticator: verify accepts valid signature", "[auth]") {
    Authenticator auth;
    auto challenge = auth.generate_challenge();

    // Generate a real ML-DSA-87 identity and sign the challenge
    auto identity = RelayIdentity::generate();
    auto sig = identity.sign(std::span<const uint8_t>(challenge.data(), challenge.size()));

    auto result = auth.verify(challenge, identity.public_key(), sig);
    REQUIRE(result.success);
    REQUIRE(result.error_code.empty());

    // namespace_hash should match identity's public_key_hash
    auto expected_hash = identity.public_key_hash();
    REQUIRE(std::equal(result.namespace_hash.begin(), result.namespace_hash.end(),
                       expected_hash.begin()));

    // public_key should be stored
    REQUIRE(result.public_key.size() == 2592);
}

TEST_CASE("Authenticator: verify rejects invalid signature", "[auth]") {
    Authenticator auth;
    auto challenge = auth.generate_challenge();

    auto identity = RelayIdentity::generate();
    auto sig = identity.sign(std::span<const uint8_t>(challenge.data(), challenge.size()));

    // Flip a byte in the signature
    sig[0] ^= 0xFF;

    auto result = auth.verify(challenge, identity.public_key(), sig);
    REQUIRE(!result.success);
    REQUIRE(result.error_code == "invalid_signature");
}

TEST_CASE("Authenticator: ACL rejects unknown key", "[auth]") {
    // Create ACL with a random namespace hash (not the signer's)
    std::array<uint8_t, 32> other_hash{};
    other_hash.fill(0xAA);
    Authenticator::KeySet acl;
    acl.insert(other_hash);

    Authenticator auth(std::move(acl));
    auto challenge = auth.generate_challenge();

    auto identity = RelayIdentity::generate();
    auto sig = identity.sign(std::span<const uint8_t>(challenge.data(), challenge.size()));

    auto result = auth.verify(challenge, identity.public_key(), sig);
    REQUIRE(!result.success);
    REQUIRE(result.error_code == "unknown_key");
}

TEST_CASE("Authenticator: ACL accepts known key", "[auth]") {
    auto identity = RelayIdentity::generate();

    // Add signer's namespace hash to ACL
    auto pk_hash = identity.public_key_hash();
    std::array<uint8_t, 32> hash_arr{};
    std::copy(pk_hash.begin(), pk_hash.end(), hash_arr.begin());

    Authenticator::KeySet acl;
    acl.insert(hash_arr);

    Authenticator auth(std::move(acl));
    auto challenge = auth.generate_challenge();

    auto sig = identity.sign(std::span<const uint8_t>(challenge.data(), challenge.size()));

    auto result = auth.verify(challenge, identity.public_key(), sig);
    REQUIRE(result.success);
    REQUIRE(result.error_code.empty());
}

TEST_CASE("Authenticator: reload_allowed_keys updates ACL", "[auth]") {
    auto identity = RelayIdentity::generate();

    // Start with empty ACL (open relay)
    Authenticator auth;
    auto challenge = auth.generate_challenge();
    auto sig = identity.sign(std::span<const uint8_t>(challenge.data(), challenge.size()));

    auto result = auth.verify(challenge, identity.public_key(), sig);
    REQUIRE(result.success);

    // Reload with restrictive ACL (random hash, not the signer)
    std::array<uint8_t, 32> other_hash{};
    other_hash.fill(0xBB);
    Authenticator::KeySet restrictive_acl;
    restrictive_acl.insert(other_hash);
    auth.reload_allowed_keys(std::move(restrictive_acl));

    // Same identity should now be rejected
    auto challenge2 = auth.generate_challenge();
    auto sig2 = identity.sign(std::span<const uint8_t>(challenge2.data(), challenge2.size()));
    auto result2 = auth.verify(challenge2, identity.public_key(), sig2);
    REQUIRE(!result2.success);
    REQUIRE(result2.error_code == "unknown_key");

    // Reload back to empty (open)
    auth.reload_allowed_keys({});
    auto challenge3 = auth.generate_challenge();
    auto sig3 = identity.sign(std::span<const uint8_t>(challenge3.data(), challenge3.size()));
    auto result3 = auth.verify(challenge3, identity.public_key(), sig3);
    REQUIRE(result3.success);
}

TEST_CASE("Authenticator: has_acl returns correct state", "[auth]") {
    Authenticator auth;
    REQUIRE(!auth.has_acl());

    std::array<uint8_t, 32> hash{};
    hash.fill(0xCC);
    Authenticator::KeySet acl;
    acl.insert(hash);
    auth.reload_allowed_keys(std::move(acl));
    REQUIRE(auth.has_acl());

    auth.reload_allowed_keys({});
    REQUIRE(!auth.has_acl());
}
