#include "db/util/blob_helpers.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

using namespace chromatindb::util;

// =============================================================================
// extract_namespace
// =============================================================================

TEST_CASE("extract_namespace returns first 32 bytes", "[blob_helpers]") {
    std::vector<uint8_t> payload(64, 0);
    for (size_t i = 0; i < 32; ++i) payload[i] = static_cast<uint8_t>(i);

    auto ns = extract_namespace(payload);
    for (size_t i = 0; i < 32; ++i) {
        REQUIRE(ns[i] == static_cast<uint8_t>(i));
    }
}

TEST_CASE("extract_namespace on exactly 32 bytes succeeds", "[blob_helpers]") {
    std::vector<uint8_t> payload(32, 0xAA);
    auto ns = extract_namespace(payload);
    for (size_t i = 0; i < 32; ++i) {
        REQUIRE(ns[i] == 0xAA);
    }
}

TEST_CASE("extract_namespace on 31 bytes throws out_of_range", "[blob_helpers]") {
    std::vector<uint8_t> payload(31, 0);
    REQUIRE_THROWS_AS(extract_namespace(payload), std::out_of_range);
}

TEST_CASE("extract_namespace on empty span throws out_of_range", "[blob_helpers]") {
    std::span<const uint8_t> empty;
    REQUIRE_THROWS_AS(extract_namespace(empty), std::out_of_range);
}

// =============================================================================
// extract_namespace_hash
// =============================================================================

TEST_CASE("extract_namespace_hash returns pair of two 32-byte arrays", "[blob_helpers]") {
    std::vector<uint8_t> payload(64, 0);
    for (size_t i = 0; i < 32; ++i) payload[i] = static_cast<uint8_t>(i);
    for (size_t i = 0; i < 32; ++i) payload[32 + i] = static_cast<uint8_t>(i + 100);

    auto [ns, hash] = extract_namespace_hash(payload);
    for (size_t i = 0; i < 32; ++i) {
        REQUIRE(ns[i] == static_cast<uint8_t>(i));
        REQUIRE(hash[i] == static_cast<uint8_t>(i + 100));
    }
}

TEST_CASE("extract_namespace_hash on exactly 64 bytes succeeds", "[blob_helpers]") {
    std::vector<uint8_t> payload(64, 0xBB);
    auto [ns, hash] = extract_namespace_hash(payload);
    for (size_t i = 0; i < 32; ++i) {
        REQUIRE(ns[i] == 0xBB);
        REQUIRE(hash[i] == 0xBB);
    }
}

TEST_CASE("extract_namespace_hash on 63 bytes throws out_of_range", "[blob_helpers]") {
    std::vector<uint8_t> payload(63, 0);
    REQUIRE_THROWS_AS(extract_namespace_hash(payload), std::out_of_range);
}

TEST_CASE("extract_namespace_hash on payload < 64 bytes throws", "[blob_helpers]") {
    std::vector<uint8_t> payload(32, 0);  // only 32 bytes, need 64
    REQUIRE_THROWS_AS(extract_namespace_hash(payload), std::out_of_range);
}

// =============================================================================
// encode_namespace_hash
// =============================================================================

TEST_CASE("encode_namespace_hash produces 64-byte vector", "[blob_helpers]") {
    std::array<uint8_t, 32> ns{};
    std::array<uint8_t, 32> hash{};
    for (size_t i = 0; i < 32; ++i) {
        ns[i] = static_cast<uint8_t>(i);
        hash[i] = static_cast<uint8_t>(i + 0x80);
    }

    auto result = encode_namespace_hash(ns, hash);
    REQUIRE(result.size() == 64);
    for (size_t i = 0; i < 32; ++i) {
        REQUIRE(result[i] == static_cast<uint8_t>(i));
        REQUIRE(result[32 + i] == static_cast<uint8_t>(i + 0x80));
    }
}

// =============================================================================
// Round-trip: encode then extract namespace_hash
// =============================================================================

TEST_CASE("Round-trip: encode_namespace_hash then extract_namespace_hash", "[blob_helpers]") {
    std::array<uint8_t, 32> ns{};
    std::array<uint8_t, 32> hash{};
    ns.fill(0x11);
    hash.fill(0x22);

    auto encoded = encode_namespace_hash(ns, hash);
    auto [ns_out, hash_out] = extract_namespace_hash(encoded);
    REQUIRE(ns_out == ns);
    REQUIRE(hash_out == hash);
}

// =============================================================================
// encode_blob_ref
// =============================================================================

TEST_CASE("encode_blob_ref produces 77 bytes with correct layout", "[blob_helpers]") {
    std::array<uint8_t, 32> ns{};
    std::array<uint8_t, 32> hash{};
    ns.fill(0xAA);
    hash.fill(0xBB);

    uint64_t seq_num = 0x0102030405060708ULL;
    uint32_t size = 0xDEADBEEF;
    bool tombstone = true;

    auto result = encode_blob_ref(ns, hash, seq_num, size, tombstone);
    REQUIRE(result.size() == 77);

    // Check namespace (bytes 0-31)
    for (size_t i = 0; i < 32; ++i) {
        REQUIRE(result[i] == 0xAA);
    }

    // Check hash (bytes 32-63)
    for (size_t i = 32; i < 64; ++i) {
        REQUIRE(result[i] == 0xBB);
    }

    // Check seq_num BE (bytes 64-71)
    REQUIRE(result[64] == 0x01);
    REQUIRE(result[65] == 0x02);
    REQUIRE(result[66] == 0x03);
    REQUIRE(result[67] == 0x04);
    REQUIRE(result[68] == 0x05);
    REQUIRE(result[69] == 0x06);
    REQUIRE(result[70] == 0x07);
    REQUIRE(result[71] == 0x08);

    // Check size BE (bytes 72-75)
    REQUIRE(result[72] == 0xDE);
    REQUIRE(result[73] == 0xAD);
    REQUIRE(result[74] == 0xBE);
    REQUIRE(result[75] == 0xEF);

    // Check tombstone flag (byte 76)
    REQUIRE(result[76] == 0x01);
}

TEST_CASE("encode_blob_ref tombstone=false produces 0x00 flag byte", "[blob_helpers]") {
    std::array<uint8_t, 32> ns{};
    std::array<uint8_t, 32> hash{};

    auto result = encode_blob_ref(ns, hash, 0, 0, false);
    REQUIRE(result.size() == 77);
    REQUIRE(result[76] == 0x00);
}
