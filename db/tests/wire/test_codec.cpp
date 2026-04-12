#include <catch2/catch_test_macros.hpp>
#include "db/wire/codec.h"
#include "db/crypto/hash.h"
#include <cstring>
#include <vector>

using namespace chromatindb::wire;

static BlobData make_test_blob() {
    BlobData blob;
    // Fill namespace with pattern
    for (size_t i = 0; i < 32; ++i) blob.namespace_id[i] = static_cast<uint8_t>(i);
    // Simulated pubkey (not real, just for codec testing)
    blob.pubkey.resize(2592, 0xAA);
    blob.data = {0x48, 0x65, 0x6C, 0x6C, 0x6F};  // "Hello"
    blob.ttl = 604800;      // 7 days
    blob.timestamp = 1709500000;
    blob.signature.resize(4627, 0xBB);
    return blob;
}

TEST_CASE("Encode/decode round-trip preserves all fields", "[codec]") {
    auto blob = make_test_blob();
    auto encoded = encode_blob(blob);
    REQUIRE(encoded.size() > 0);

    auto decoded = decode_blob(encoded);

    REQUIRE(decoded.namespace_id == blob.namespace_id);
    REQUIRE(decoded.pubkey == blob.pubkey);
    REQUIRE(decoded.data == blob.data);
    REQUIRE(decoded.ttl == blob.ttl);
    REQUIRE(decoded.timestamp == blob.timestamp);
    REQUIRE(decoded.signature == blob.signature);
}

TEST_CASE("Encode/decode preserves empty data field", "[codec]") {
    BlobData blob;
    blob.namespace_id.fill(0x42);
    blob.pubkey.resize(2592, 0x01);  // Must be Signer::PUBLIC_KEY_SIZE
    blob.data = {};  // empty
    blob.ttl = 0;
    blob.timestamp = 0;
    blob.signature.resize(4627, 0x06);

    auto encoded = encode_blob(blob);
    auto decoded = decode_blob(encoded);

    REQUIRE(decoded.data.empty());
    REQUIRE(decoded.ttl == 0);
    REQUIRE(decoded.timestamp == 0);
}

TEST_CASE("Encode produces deterministic output", "[codec]") {
    auto blob = make_test_blob();
    auto enc1 = encode_blob(blob);
    auto enc2 = encode_blob(blob);
    REQUIRE(enc1 == enc2);
}

TEST_CASE("Encode -> decode -> encode produces identical bytes (canonicality)", "[codec]") {
    auto blob = make_test_blob();
    auto enc1 = encode_blob(blob);
    auto decoded = decode_blob(enc1);
    auto enc2 = encode_blob(decoded);
    REQUIRE(enc1 == enc2);
}

TEST_CASE("ForceDefaults encodes zero-value ttl", "[codec]") {
    BlobData blob;
    blob.namespace_id.fill(0);
    blob.pubkey.resize(2592, 0x00);  // Must be Signer::PUBLIC_KEY_SIZE
    blob.ttl = 0;  // zero should still be encoded
    blob.timestamp = 0;

    auto encoded = encode_blob(blob);
    auto decoded = decode_blob(encoded);
    REQUIRE(decoded.ttl == 0);
}

TEST_CASE("build_signing_input produces SHA3-256 of canonical concatenation", "[codec]") {
    std::array<uint8_t, 32> ns{};
    ns[0] = 0xFF;
    ns[31] = 0x01;
    std::vector<uint8_t> data = {0xAA, 0xBB};
    uint32_t ttl = 604800;       // 0x00093A80
    uint64_t timestamp = 1709500000;  // 0x0000000065E4A660

    auto input = build_signing_input(ns, data, ttl, timestamp);

    // Returns a 32-byte SHA3-256 digest
    REQUIRE(input.size() == 32);

    // Build expected: SHA3-256 of the canonical concatenation
    // namespace(32) || data(var) || ttl_be(4) || timestamp_be(8)
    std::vector<uint8_t> concat;
    concat.insert(concat.end(), ns.begin(), ns.end());
    concat.insert(concat.end(), data.begin(), data.end());
    // TTL as big-endian uint32: 604800 = 0x00093A80
    concat.push_back(static_cast<uint8_t>(ttl >> 24));
    concat.push_back(static_cast<uint8_t>(ttl >> 16));
    concat.push_back(static_cast<uint8_t>(ttl >> 8));
    concat.push_back(static_cast<uint8_t>(ttl));
    // Timestamp as big-endian uint64: 1709500000 = 0x0000000065E4A660
    concat.push_back(static_cast<uint8_t>(timestamp >> 56));
    concat.push_back(static_cast<uint8_t>(timestamp >> 48));
    concat.push_back(static_cast<uint8_t>(timestamp >> 40));
    concat.push_back(static_cast<uint8_t>(timestamp >> 32));
    concat.push_back(static_cast<uint8_t>(timestamp >> 24));
    concat.push_back(static_cast<uint8_t>(timestamp >> 16));
    concat.push_back(static_cast<uint8_t>(timestamp >> 8));
    concat.push_back(static_cast<uint8_t>(timestamp));

    auto expected = chromatindb::crypto::sha3_256(concat);
    REQUIRE(input == expected);
}

TEST_CASE("build_signing_input is independent of FlatBuffer encoding", "[codec]") {
    auto blob = make_test_blob();
    auto encoded = encode_blob(blob);

    auto signing_input = build_signing_input(
        blob.namespace_id, blob.data, blob.ttl, blob.timestamp);

    // Signing input is a 32-byte SHA3-256 digest, unrelated to FlatBuffer layout
    REQUIRE(signing_input.size() == 32);
    REQUIRE(signing_input.size() != encoded.size());

    // Hash of encoded blob is a different hash (over different input)
    auto encoded_hash = blob_hash(encoded);
    REQUIRE(signing_input != encoded_hash);
}

TEST_CASE("build_signing_input is deterministic for same logical content", "[codec]") {
    std::array<uint8_t, 32> ns{};
    ns.fill(0x42);
    std::vector<uint8_t> data = {1, 2, 3};

    auto s1 = build_signing_input(ns, data, 100, 200);
    auto s2 = build_signing_input(ns, data, 100, 200);
    REQUIRE(s1 == s2);
}

TEST_CASE("decode_blob rejects wrong pubkey size", "[codec]") {
    // Create a blob with wrong pubkey size, encode it, then decode should throw
    chromatindb::wire::BlobData blob;
    blob.namespace_id.fill(0x01);
    blob.pubkey.assign(100, 0xAA);  // Wrong size (not 2592)
    blob.data = {1, 2, 3};
    blob.ttl = 3600;
    blob.timestamp = 1000;
    blob.signature.assign(4627, 0xBB);

    auto encoded = chromatindb::wire::encode_blob(blob);
    REQUIRE_THROWS_AS(
        chromatindb::wire::decode_blob(encoded),
        std::runtime_error);
}

TEST_CASE("blob_hash produces SHA3-256 of full encoded blob", "[codec]") {
    auto blob = make_test_blob();
    auto encoded = encode_blob(blob);

    auto hash = blob_hash(encoded);
    auto expected = chromatindb::crypto::sha3_256(std::span<const uint8_t>(encoded));

    REQUIRE(hash == expected);
}

TEST_CASE("blob_hash differs for different signatures on same data", "[codec]") {
    auto blob1 = make_test_blob();
    auto blob2 = make_test_blob();
    blob2.signature[0] = 0xCC;  // Different signature

    auto enc1 = encode_blob(blob1);
    auto enc2 = encode_blob(blob2);

    auto hash1 = blob_hash(enc1);
    auto hash2 = blob_hash(enc2);

    REQUIRE(hash1 != hash2);
}

TEST_CASE("blob_hash is deterministic", "[codec]") {
    auto blob = make_test_blob();
    auto encoded = encode_blob(blob);

    auto h1 = blob_hash(encoded);
    auto h2 = blob_hash(encoded);
    REQUIRE(h1 == h2);
}

// =============================================================================
// saturating_expiry tests
// =============================================================================

TEST_CASE("saturating_expiry normal addition", "[codec][ttl][saturating]") {
    REQUIRE(saturating_expiry(1000, 100) == 1100);
}

TEST_CASE("saturating_expiry overflow clamps to UINT64_MAX", "[codec][ttl][saturating]") {
    REQUIRE(saturating_expiry(UINT64_MAX - 10, 20) == UINT64_MAX);
}

TEST_CASE("saturating_expiry permanent returns 0", "[codec][ttl][saturating]") {
    REQUIRE(saturating_expiry(1000, 0) == 0);
    REQUIRE(saturating_expiry(0, 0) == 0);
}

TEST_CASE("saturating_expiry max timestamp with ttl=1", "[codec][ttl][saturating]") {
    REQUIRE(saturating_expiry(UINT64_MAX, 1) == UINT64_MAX);
}

// =============================================================================
// is_blob_expired tests
// =============================================================================

TEST_CASE("is_blob_expired permanent never expires", "[codec][ttl]") {
    BlobData blob;
    blob.ttl = 0;
    blob.timestamp = 1000;
    REQUIRE_FALSE(is_blob_expired(blob, 0));
    REQUIRE_FALSE(is_blob_expired(blob, UINT64_MAX));
}

TEST_CASE("is_blob_expired within TTL", "[codec][ttl]") {
    BlobData blob;
    blob.ttl = 100;
    blob.timestamp = 1000;
    REQUIRE_FALSE(is_blob_expired(blob, 1099));
}

TEST_CASE("is_blob_expired after TTL", "[codec][ttl]") {
    BlobData blob;
    blob.ttl = 100;
    blob.timestamp = 1000;
    REQUIRE(is_blob_expired(blob, 1200));
}

TEST_CASE("is_blob_expired at exact boundary", "[codec][ttl]") {
    BlobData blob;
    blob.ttl = 100;
    blob.timestamp = 1000;
    // Expired at equality per D-06: saturating_expiry(1000, 100) = 1100, 1100 <= 1100
    REQUIRE(is_blob_expired(blob, 1100));
}

TEST_CASE("is_blob_expired overflow blob never expires", "[codec][ttl]") {
    BlobData blob;
    blob.ttl = 20;
    blob.timestamp = UINT64_MAX - 10;
    // saturating_expiry clamps to UINT64_MAX, so UINT64_MAX <= any_now is only true
    // when now == UINT64_MAX, but that would mean the blob is "effectively permanent"
    // Actually per the spec: overflow clamps to UINT64_MAX means UINT64_MAX <= now
    // is true when now == UINT64_MAX. But the intent is "effectively permanent".
    // Let's check: saturating_expiry(UINT64_MAX-10, 20) == UINT64_MAX.
    // is_blob_expired checks saturating_expiry <= now. If now < UINT64_MAX, false.
    REQUIRE_FALSE(is_blob_expired(blob, UINT64_MAX - 1));
    REQUIRE_FALSE(is_blob_expired(blob, 0));
}
