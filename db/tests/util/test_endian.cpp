#include "db/util/endian.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <vector>

using namespace chromatindb::util;

// =============================================================================
// write_u16_be
// =============================================================================

TEST_CASE("write_u16_be appends 2 bytes in BE order", "[endian]") {
    std::vector<uint8_t> buf;
    write_u16_be(buf, 0x1234);
    REQUIRE(buf.size() == 2);
    REQUIRE(buf[0] == 0x12);
    REQUIRE(buf[1] == 0x34);
}

// =============================================================================
// write_u32_be
// =============================================================================

TEST_CASE("write_u32_be appends 4 bytes in BE order", "[endian]") {
    std::vector<uint8_t> buf;
    write_u32_be(buf, 0xDEADBEEF);
    REQUIRE(buf.size() == 4);
    REQUIRE(buf[0] == 0xDE);
    REQUIRE(buf[1] == 0xAD);
    REQUIRE(buf[2] == 0xBE);
    REQUIRE(buf[3] == 0xEF);
}

// =============================================================================
// write_u64_be
// =============================================================================

TEST_CASE("write_u64_be appends 8 bytes in BE order", "[endian]") {
    std::vector<uint8_t> buf;
    write_u64_be(buf, 0x0102030405060708ULL);
    REQUIRE(buf.size() == 8);
    REQUIRE(buf[0] == 0x01);
    REQUIRE(buf[1] == 0x02);
    REQUIRE(buf[2] == 0x03);
    REQUIRE(buf[3] == 0x04);
    REQUIRE(buf[4] == 0x05);
    REQUIRE(buf[5] == 0x06);
    REQUIRE(buf[6] == 0x07);
    REQUIRE(buf[7] == 0x08);
}

// =============================================================================
// store_u32_be
// =============================================================================

TEST_CASE("store_u32_be writes 4 bytes in BE to pre-allocated buffer", "[endian]") {
    uint8_t out[4] = {};
    store_u32_be(out, 0xCAFEBABE);
    REQUIRE(out[0] == 0xCA);
    REQUIRE(out[1] == 0xFE);
    REQUIRE(out[2] == 0xBA);
    REQUIRE(out[3] == 0xBE);
}

// =============================================================================
// store_u64_be
// =============================================================================

TEST_CASE("store_u64_be writes 8 bytes in BE to pre-allocated buffer", "[endian]") {
    uint8_t out[8] = {};
    store_u64_be(out, 0xFEDCBA9876543210ULL);
    REQUIRE(out[0] == 0xFE);
    REQUIRE(out[1] == 0xDC);
    REQUIRE(out[2] == 0xBA);
    REQUIRE(out[3] == 0x98);
    REQUIRE(out[4] == 0x76);
    REQUIRE(out[5] == 0x54);
    REQUIRE(out[6] == 0x32);
    REQUIRE(out[7] == 0x10);
}

// =============================================================================
// read_u16_be (span) -- bounds checked
// =============================================================================

TEST_CASE("read_u16_be(span) reads 2-byte BE", "[endian]") {
    std::vector<uint8_t> data = {0x12, 0x34};
    REQUIRE(read_u16_be(std::span<const uint8_t>(data)) == 0x1234);
}

TEST_CASE("read_u16_be(span) throws on span < 2 bytes", "[endian]") {
    std::vector<uint8_t> data = {0x12};
    REQUIRE_THROWS_AS(read_u16_be(std::span<const uint8_t>(data)), std::out_of_range);
}

TEST_CASE("read_u16_be(span) throws on empty span", "[endian]") {
    std::span<const uint8_t> empty;
    REQUIRE_THROWS_AS(read_u16_be(empty), std::out_of_range);
}

// =============================================================================
// read_u32_be (span) -- bounds checked
// =============================================================================

TEST_CASE("read_u32_be(span) reads 4-byte BE", "[endian]") {
    std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};
    REQUIRE(read_u32_be(std::span<const uint8_t>(data)) == 0xDEADBEEF);
}

TEST_CASE("read_u32_be(span) throws on span < 4 bytes", "[endian]") {
    std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE};
    REQUIRE_THROWS_AS(read_u32_be(std::span<const uint8_t>(data)), std::out_of_range);
}

TEST_CASE("read_u32_be(span) throws on empty span", "[endian]") {
    std::span<const uint8_t> empty;
    REQUIRE_THROWS_AS(read_u32_be(empty), std::out_of_range);
}

// =============================================================================
// read_u64_be (span) -- bounds checked
// =============================================================================

TEST_CASE("read_u64_be(span) reads 8-byte BE", "[endian]") {
    std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    REQUIRE(read_u64_be(std::span<const uint8_t>(data)) == 0x0102030405060708ULL);
}

TEST_CASE("read_u64_be(span) throws on span < 8 bytes", "[endian]") {
    std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    REQUIRE_THROWS_AS(read_u64_be(std::span<const uint8_t>(data)), std::out_of_range);
}

TEST_CASE("read_u64_be(span) throws on empty span", "[endian]") {
    std::span<const uint8_t> empty;
    REQUIRE_THROWS_AS(read_u64_be(empty), std::out_of_range);
}

// =============================================================================
// read_u32_be (raw pointer) -- unchecked
// =============================================================================

TEST_CASE("read_u32_be(ptr) reads 4-byte BE", "[endian]") {
    uint8_t data[] = {0xCA, 0xFE, 0xBA, 0xBE};
    REQUIRE(read_u32_be(data) == 0xCAFEBABE);
}

// =============================================================================
// read_u64_be (raw pointer) -- unchecked
// =============================================================================

TEST_CASE("read_u64_be(ptr) reads 8-byte BE", "[endian]") {
    uint8_t data[] = {0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10};
    REQUIRE(read_u64_be(data) == 0xFEDCBA9876543210ULL);
}

// =============================================================================
// Round-trip tests
// =============================================================================

TEST_CASE("Round-trip: write_u16_be then read_u16_be", "[endian]") {
    std::vector<uint8_t> buf;
    write_u16_be(buf, 0xABCD);
    REQUIRE(read_u16_be(std::span<const uint8_t>(buf)) == 0xABCD);
}

TEST_CASE("Round-trip: write_u32_be then read_u32_be", "[endian]") {
    std::vector<uint8_t> buf;
    write_u32_be(buf, 0x12345678);
    REQUIRE(read_u32_be(std::span<const uint8_t>(buf)) == 0x12345678);
}

TEST_CASE("Round-trip: write_u64_be then read_u64_be", "[endian]") {
    std::vector<uint8_t> buf;
    write_u64_be(buf, 0xFEDCBA9876543210ULL);
    REQUIRE(read_u64_be(std::span<const uint8_t>(buf)) == 0xFEDCBA9876543210ULL);
}

TEST_CASE("Round-trip: store_u32_be then read_u32_be(ptr)", "[endian]") {
    uint8_t out[4];
    store_u32_be(out, 0xDEADBEEF);
    REQUIRE(read_u32_be(out) == 0xDEADBEEF);
}

TEST_CASE("Round-trip: store_u64_be then read_u64_be(ptr)", "[endian]") {
    uint8_t out[8];
    store_u64_be(out, 0x0102030405060708ULL);
    REQUIRE(read_u64_be(out) == 0x0102030405060708ULL);
}

// =============================================================================
// Boundary values
// =============================================================================

TEST_CASE("Boundary: u16 zero and max", "[endian]") {
    std::vector<uint8_t> buf;
    write_u16_be(buf, 0);
    write_u16_be(buf, UINT16_MAX);
    REQUIRE(read_u16_be(std::span<const uint8_t>(buf.data(), 2)) == 0);
    REQUIRE(read_u16_be(std::span<const uint8_t>(buf.data() + 2, 2)) == UINT16_MAX);
}

TEST_CASE("Boundary: u32 zero and max", "[endian]") {
    std::vector<uint8_t> buf;
    write_u32_be(buf, 0);
    write_u32_be(buf, UINT32_MAX);
    REQUIRE(read_u32_be(std::span<const uint8_t>(buf.data(), 4)) == 0);
    REQUIRE(read_u32_be(std::span<const uint8_t>(buf.data() + 4, 4)) == UINT32_MAX);
}

TEST_CASE("Boundary: u64 zero and max", "[endian]") {
    std::vector<uint8_t> buf;
    write_u64_be(buf, 0);
    write_u64_be(buf, UINT64_MAX);
    REQUIRE(read_u64_be(std::span<const uint8_t>(buf.data(), 8)) == 0);
    REQUIRE(read_u64_be(std::span<const uint8_t>(buf.data() + 8, 8)) == UINT64_MAX);
}
