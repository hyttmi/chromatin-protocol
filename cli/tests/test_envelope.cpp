#include <catch2/catch_test_macros.hpp>
#include "cli/src/envelope.h"
#include "cli/src/identity.h"
#include "cli/src/wire.h"  // to_hex

#include <oqs/sha3.h>
#include <algorithm>
#include <cstring>
#include <numeric>

using namespace chromatindb::cli;
using namespace chromatindb::cli::envelope;

TEST_CASE("envelope: encrypt for self, decrypt roundtrip", "[envelope]") {
    auto alice = Identity::generate();

    std::vector<uint8_t> plaintext = {1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<std::span<const uint8_t>> recipients = {alice.kem_pubkey()};

    auto env = encrypt(plaintext, recipients);

    // Verify magic bytes "CENV"
    REQUIRE(env.size() >= 20);
    REQUIRE(env[0] == 0x43);
    REQUIRE(env[1] == 0x45);
    REQUIRE(env[2] == 0x4E);
    REQUIRE(env[3] == 0x56);

    // Verify version and suite
    REQUIRE(env[4] == 0x01);
    REQUIRE(env[5] == 0x01);

    // Verify recipient count
    REQUIRE(load_u16_be(env.data() + 6) == 1);

    // Decrypt
    auto result = decrypt(env, alice.kem_seckey(), alice.kem_pubkey());
    REQUIRE(result.has_value());
    REQUIRE(result->size() == plaintext.size());
    REQUIRE(std::equal(result->begin(), result->end(), plaintext.begin()));
}

TEST_CASE("envelope: encrypt for two recipients, both can decrypt", "[envelope]") {
    auto alice = Identity::generate();
    auto bob = Identity::generate();

    std::vector<uint8_t> plaintext = {0xCA, 0xFE, 0xBA, 0xBE};
    std::vector<std::span<const uint8_t>> recipients = {
        alice.kem_pubkey(), bob.kem_pubkey()
    };

    auto env = encrypt(plaintext, recipients);

    // Verify 2 stanzas
    REQUIRE(load_u16_be(env.data() + 6) == 2);

    // Alice decrypts
    auto alice_result = decrypt(env, alice.kem_seckey(), alice.kem_pubkey());
    REQUIRE(alice_result.has_value());
    REQUIRE(*alice_result == plaintext);

    // Bob decrypts
    auto bob_result = decrypt(env, bob.kem_seckey(), bob.kem_pubkey());
    REQUIRE(bob_result.has_value());
    REQUIRE(*bob_result == plaintext);
}

TEST_CASE("envelope: non-recipient cannot decrypt", "[envelope]") {
    auto alice = Identity::generate();
    auto eve = Identity::generate();

    std::vector<uint8_t> plaintext = {1, 2, 3};
    std::vector<std::span<const uint8_t>> recipients = {alice.kem_pubkey()};

    auto env = encrypt(plaintext, recipients);

    // Eve is not a recipient
    auto result = decrypt(env, eve.kem_seckey(), eve.kem_pubkey());
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("envelope: stanzas sorted by kem_pk_hash", "[envelope]") {
    auto alice = Identity::generate();
    auto bob = Identity::generate();
    auto carol = Identity::generate();

    std::vector<uint8_t> plaintext = {42};
    std::vector<std::span<const uint8_t>> recipients = {
        alice.kem_pubkey(), bob.kem_pubkey(), carol.kem_pubkey()
    };

    auto env = encrypt(plaintext, recipients);
    REQUIRE(load_u16_be(env.data() + 6) == 3);

    // Extract pk hashes from the envelope stanzas and verify sorted order
    const uint8_t* stanza_start = env.data() + 20;
    std::array<uint8_t, 32> hash0{}, hash1{}, hash2{};
    std::memcpy(hash0.data(), stanza_start, 32);
    std::memcpy(hash1.data(), stanza_start + 1648, 32);
    std::memcpy(hash2.data(), stanza_start + 1648 * 2, 32);

    // Each hash must be <= the next (lexicographic)
    REQUIRE(hash0 <= hash1);
    REQUIRE(hash1 <= hash2);

    // All three can still decrypt
    REQUIRE(decrypt(env, alice.kem_seckey(), alice.kem_pubkey()).has_value());
    REQUIRE(decrypt(env, bob.kem_seckey(), bob.kem_pubkey()).has_value());
    REQUIRE(decrypt(env, carol.kem_seckey(), carol.kem_pubkey()).has_value());
}

TEST_CASE("envelope: is_envelope detects magic", "[envelope]") {
    auto id = Identity::generate();

    std::vector<uint8_t> plaintext = {99};
    std::vector<std::span<const uint8_t>> recipients = {id.kem_pubkey()};
    auto env = encrypt(plaintext, recipients);

    REQUIRE(is_envelope(env));

    // Random data is not an envelope
    std::vector<uint8_t> random_data = {0x00, 0x01, 0x02, 0x03, 0x04};
    REQUIRE_FALSE(is_envelope(random_data));

    // Empty data
    std::vector<uint8_t> empty;
    REQUIRE_FALSE(is_envelope(empty));

    // Almost the magic but not quite
    std::vector<uint8_t> almost = {0x43, 0x45, 0x4E, 0x57};
    REQUIRE_FALSE(is_envelope(almost));
}

TEST_CASE("envelope: empty plaintext roundtrip", "[envelope]") {
    auto id = Identity::generate();

    std::vector<uint8_t> plaintext;  // empty
    std::vector<std::span<const uint8_t>> recipients = {id.kem_pubkey()};

    auto env = encrypt(plaintext, recipients);

    // Envelope should contain header(20) + 1 stanza(1648) + tag(16) = 1684
    REQUIRE(env.size() == 20 + 1648 + 16);

    auto result = decrypt(env, id.kem_seckey(), id.kem_pubkey());
    REQUIRE(result.has_value());
    REQUIRE(result->empty());
}

TEST_CASE("envelope: large plaintext roundtrip", "[envelope]") {
    auto id = Identity::generate();

    // 1 MiB plaintext
    std::vector<uint8_t> plaintext(1024 * 1024);
    std::iota(plaintext.begin(), plaintext.end(), static_cast<uint8_t>(0));

    std::vector<std::span<const uint8_t>> recipients = {id.kem_pubkey()};
    auto env = encrypt(plaintext, recipients);

    // Expected size: header(20) + 1 stanza(1648) + plaintext(1M) + tag(16)
    REQUIRE(env.size() == 20 + 1648 + plaintext.size() + 16);

    auto result = decrypt(env, id.kem_seckey(), id.kem_pubkey());
    REQUIRE(result.has_value());
    REQUIRE(result->size() == plaintext.size());
    REQUIRE(*result == plaintext);
}
