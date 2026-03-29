/// chromatindb_test_vectors -- generate JSON known-answer test vectors for
/// all crypto primitives used by the chromatindb protocol.
///
/// These vectors bridge C++ and Python: if both produce the same output for
/// the same input, cross-language interoperability is guaranteed.
///
/// Output: JSON to stdout with sections for SHA3-256, HKDF-SHA256,
/// ChaCha20-Poly1305, build_signing_input, ML-DSA-87, and namespace derivation.

#include "db/crypto/hash.h"
#include "db/crypto/kdf.h"
#include "db/crypto/aead.h"
#include "db/crypto/signing.h"
#include "db/wire/codec.h"
#include "db/util/hex.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>

namespace {

using chromatindb::util::to_hex;
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// SHA3-256 vectors
// ---------------------------------------------------------------------------

json generate_sha3_vectors() {
    json vectors = json::array();

    // Vector 1: empty input (NIST reference)
    {
        std::vector<uint8_t> empty;
        auto hash = chromatindb::crypto::sha3_256(empty);
        vectors.push_back({
            {"input_hex", ""},
            {"expected_hex", to_hex(hash)}
        });
    }

    // Vector 2: "chromatindb" (ASCII 0x6368726f6d6174696e6462)
    {
        const std::string input_str = "chromatindb";
        std::vector<uint8_t> input(input_str.begin(), input_str.end());
        auto hash = chromatindb::crypto::sha3_256(input);
        vectors.push_back({
            {"input_hex", to_hex(std::span<const uint8_t>(input))},
            {"expected_hex", to_hex(hash)}
        });
    }

    // Vector 3: 0x00..0xFF (256 bytes, all byte values)
    {
        std::vector<uint8_t> input(256);
        for (int i = 0; i < 256; ++i) {
            input[static_cast<size_t>(i)] = static_cast<uint8_t>(i);
        }
        auto hash = chromatindb::crypto::sha3_256(input);
        vectors.push_back({
            {"input_hex", to_hex(std::span<const uint8_t>(input))},
            {"expected_hex", to_hex(hash)}
        });
    }

    return vectors;
}

// ---------------------------------------------------------------------------
// HKDF-SHA256 vectors
// ---------------------------------------------------------------------------

json generate_hkdf_vectors() {
    json vectors = json::array();

    // Vector 1: with salt (deterministic inputs)
    {
        std::vector<uint8_t> salt(32);
        for (size_t i = 0; i < 32; ++i) {
            salt[i] = static_cast<uint8_t>(i);
        }
        std::vector<uint8_t> ikm(22, 0x0b);
        const std::string context = "chromatindb-test";
        constexpr size_t output_len = 32;

        auto prk = chromatindb::crypto::KDF::extract(salt, ikm);
        auto okm = chromatindb::crypto::KDF::expand(prk.span(), context, output_len);

        vectors.push_back({
            {"description", "with salt"},
            {"salt_hex", to_hex(std::span<const uint8_t>(salt))},
            {"ikm_hex", to_hex(std::span<const uint8_t>(ikm))},
            {"context", context},
            {"output_len", output_len},
            {"prk_hex", to_hex(prk.span())},
            {"okm_hex", to_hex(okm.span())}
        });
    }

    // Vector 2: empty salt (critical -- SDK MUST handle this correctly)
    // The C++ code passes salt.size()=0 to libsodium, which internally
    // uses zero-filled 32-byte salt per RFC 5869.
    {
        std::vector<uint8_t> empty_salt;
        std::vector<uint8_t> ikm(22, 0x0b);
        const std::string context = "chromatindb-dare-v1";
        constexpr size_t output_len = 32;

        auto prk = chromatindb::crypto::KDF::extract(empty_salt, ikm);
        auto okm = chromatindb::crypto::KDF::expand(prk.span(), context, output_len);

        vectors.push_back({
            {"description", "empty salt (critical -- SDK MUST handle this correctly)"},
            {"salt_hex", ""},
            {"ikm_hex", to_hex(std::span<const uint8_t>(ikm))},
            {"context", context},
            {"output_len", output_len},
            {"prk_hex", to_hex(prk.span())},
            {"okm_hex", to_hex(okm.span())}
        });
    }

    // Vector 3: long output (64 bytes, tests multi-block expand)
    {
        std::vector<uint8_t> salt = {0xde, 0xad, 0xbe, 0xef};
        std::vector<uint8_t> ikm = {0xca, 0xfe, 0xba, 0xbe};
        const std::string context = "chromatindb-test-long";
        constexpr size_t output_len = 64;

        auto prk = chromatindb::crypto::KDF::extract(salt, ikm);
        auto okm = chromatindb::crypto::KDF::expand(prk.span(), context, output_len);

        vectors.push_back({
            {"description", "long output (64 bytes, tests multi-block expand)"},
            {"salt_hex", to_hex(std::span<const uint8_t>(salt))},
            {"ikm_hex", to_hex(std::span<const uint8_t>(ikm))},
            {"context", context},
            {"output_len", output_len},
            {"prk_hex", to_hex(prk.span())},
            {"okm_hex", to_hex(okm.span())}
        });
    }

    return vectors;
}

// ---------------------------------------------------------------------------
// ChaCha20-Poly1305 AEAD vectors
// ---------------------------------------------------------------------------

json generate_aead_vectors() {
    json vectors = json::array();

    // Common key: 0x00..0x1F (32 bytes)
    std::array<uint8_t, 32> key{};
    for (size_t i = 0; i < 32; ++i) {
        key[i] = static_cast<uint8_t>(i);
    }

    // Vector 1: basic encrypt/decrypt
    {
        std::array<uint8_t, 12> nonce{};
        nonce[11] = 1; // 0x000000000000000000000001

        const std::string pt_str = "Hello, chromatindb";
        std::vector<uint8_t> plaintext(pt_str.begin(), pt_str.end());

        const std::string ad_str = "additional data";
        std::vector<uint8_t> ad(ad_str.begin(), ad_str.end());

        auto ciphertext = chromatindb::crypto::AEAD::encrypt(plaintext, ad, nonce, key);

        vectors.push_back({
            {"description", "basic encrypt/decrypt"},
            {"key_hex", to_hex(key)},
            {"nonce_hex", to_hex(nonce)},
            {"plaintext_hex", to_hex(std::span<const uint8_t>(plaintext))},
            {"ad_hex", to_hex(std::span<const uint8_t>(ad))},
            {"ciphertext_hex", to_hex(std::span<const uint8_t>(ciphertext))}
        });
    }

    // Vector 2: empty plaintext
    {
        std::array<uint8_t, 12> nonce{};
        nonce[11] = 2; // 0x000000000000000000000002

        std::vector<uint8_t> plaintext;
        std::vector<uint8_t> ad;

        auto ciphertext = chromatindb::crypto::AEAD::encrypt(plaintext, ad, nonce, key);

        vectors.push_back({
            {"description", "empty plaintext"},
            {"key_hex", to_hex(key)},
            {"nonce_hex", to_hex(nonce)},
            {"plaintext_hex", ""},
            {"ad_hex", ""},
            {"ciphertext_hex", to_hex(std::span<const uint8_t>(ciphertext))}
        });
    }

    // Vector 3: empty ad
    {
        std::array<uint8_t, 12> nonce{};
        nonce[11] = 3; // 0x000000000000000000000003

        const std::string pt_str = "test data";
        std::vector<uint8_t> plaintext(pt_str.begin(), pt_str.end());
        std::vector<uint8_t> ad;

        auto ciphertext = chromatindb::crypto::AEAD::encrypt(plaintext, ad, nonce, key);

        vectors.push_back({
            {"description", "empty ad"},
            {"key_hex", to_hex(key)},
            {"nonce_hex", to_hex(nonce)},
            {"plaintext_hex", to_hex(std::span<const uint8_t>(plaintext))},
            {"ad_hex", ""},
            {"ciphertext_hex", to_hex(std::span<const uint8_t>(ciphertext))}
        });
    }

    return vectors;
}

// ---------------------------------------------------------------------------
// build_signing_input vectors
// ---------------------------------------------------------------------------

json generate_signing_input_vectors() {
    json vectors = json::array();

    // Vector 1: basic signing input (32 zero-byte namespace)
    {
        std::array<uint8_t, 32> ns{};
        const std::string data_str = "Hello";
        std::vector<uint8_t> data(data_str.begin(), data_str.end());
        uint32_t ttl = 3600;
        uint64_t timestamp = 1700000000;

        auto digest = chromatindb::wire::build_signing_input(ns, data, ttl, timestamp);

        vectors.push_back({
            {"description", "basic signing input"},
            {"namespace_hex", to_hex(ns)},
            {"data_hex", to_hex(std::span<const uint8_t>(data))},
            {"ttl", ttl},
            {"timestamp", timestamp},
            {"expected_hex", to_hex(digest)}
        });
    }

    // Vector 2: empty data, 0xFF namespace
    {
        std::array<uint8_t, 32> ns{};
        std::fill(ns.begin(), ns.end(), 0xFF);
        std::vector<uint8_t> data;
        uint32_t ttl = 0;
        uint64_t timestamp = 0;

        auto digest = chromatindb::wire::build_signing_input(ns, data, ttl, timestamp);

        vectors.push_back({
            {"description", "empty data"},
            {"namespace_hex", to_hex(ns)},
            {"data_hex", ""},
            {"ttl", ttl},
            {"timestamp", timestamp},
            {"expected_hex", to_hex(digest)}
        });
    }

    // Vector 3: realistic values (namespace derived from a generated pubkey)
    {
        // Use a fixed-pattern namespace (SHA3-256 of "test-namespace-key")
        const std::string key_str = "test-namespace-key";
        std::vector<uint8_t> key_bytes(key_str.begin(), key_str.end());
        auto ns = chromatindb::crypto::sha3_256(key_bytes);

        const std::string data_str = "real blob data";
        std::vector<uint8_t> data(data_str.begin(), data_str.end());
        uint32_t ttl = 86400;
        uint64_t timestamp = 1711929600;

        auto digest = chromatindb::wire::build_signing_input(ns, data, ttl, timestamp);

        vectors.push_back({
            {"description", "realistic values"},
            {"namespace_hex", to_hex(ns)},
            {"data_hex", to_hex(std::span<const uint8_t>(data))},
            {"ttl", ttl},
            {"timestamp", timestamp},
            {"expected_hex", to_hex(digest)}
        });
    }

    return vectors;
}

// ---------------------------------------------------------------------------
// ML-DSA-87 vectors
// ---------------------------------------------------------------------------

json generate_ml_dsa_vectors() {
    chromatindb::crypto::Signer signer;
    signer.generate_keypair();

    auto pubkey = signer.export_public_key();
    auto seckey = signer.export_secret_key();

    // Sign a known 32-byte message (SHA3-256 of "test")
    const std::string test_str = "test";
    std::vector<uint8_t> test_bytes(test_str.begin(), test_str.end());
    auto message = chromatindb::crypto::sha3_256(test_bytes);

    auto signature = signer.sign(message);

    // Verify the signature
    bool valid = chromatindb::crypto::Signer::verify(message, signature, pubkey);

    json result;
    result["description"] = "ML-DSA-87 keypair + sign + verify";
    result["public_key_hex"] = to_hex(pubkey);
    result["secret_key_hex"] = to_hex(seckey);
    result["message_hex"] = to_hex(message);
    result["signature_hex"] = to_hex(std::span<const uint8_t>(signature));
    result["verify_result"] = valid;
    result["public_key_size"] = chromatindb::crypto::Signer::PUBLIC_KEY_SIZE;
    result["secret_key_size"] = chromatindb::crypto::Signer::SECRET_KEY_SIZE;

    return result;
}

// ---------------------------------------------------------------------------
// Namespace derivation vector
// ---------------------------------------------------------------------------

json generate_namespace_vector(std::span<const uint8_t> pubkey) {
    auto ns = chromatindb::crypto::sha3_256(pubkey);

    json result;
    result["description"] = "namespace = SHA3-256(public_key)";
    result["public_key_hex"] = to_hex(pubkey);
    result["namespace_hex"] = to_hex(ns);

    return result;
}

} // anonymous namespace

int main() {
    json output;

    output["sha3_256"] = generate_sha3_vectors();
    output["hkdf"] = generate_hkdf_vectors();
    output["aead"] = generate_aead_vectors();
    output["signing_input"] = generate_signing_input_vectors();

    // ML-DSA-87 generates a keypair -- we need the pubkey for namespace derivation
    auto ml_dsa = generate_ml_dsa_vectors();
    output["ml_dsa_87"] = ml_dsa;

    // Namespace derivation uses the same pubkey from ML-DSA-87 section
    auto pubkey_hex = ml_dsa["public_key_hex"].get<std::string>();
    auto pubkey_bytes = chromatindb::util::from_hex(pubkey_hex);
    output["namespace_derivation"] = generate_namespace_vector(pubkey_bytes);

    std::cout << output.dump(2) << std::endl;

    return 0;
}
