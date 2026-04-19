#include <catch2/catch_test_macros.hpp>
#include "cli/src/wire.h"

#include <flatbuffers/flatbuffers.h>

#include <array>
#include <cstring>
#include <string>
#include <vector>

using namespace chromatindb::cli;

namespace {

ManifestData make_valid_manifest(uint32_t n_chunks = 2) {
    ManifestData m;
    m.version               = MANIFEST_VERSION_V1;
    m.chunk_size_bytes      = CHUNK_SIZE_BYTES_DEFAULT;
    m.segment_count         = n_chunks;
    m.total_plaintext_bytes = static_cast<uint64_t>(n_chunks) * 1024;
    for (size_t i = 0; i < 32; ++i) m.plaintext_sha3[i] = static_cast<uint8_t>(i + 1);
    m.chunk_hashes.resize(static_cast<size_t>(n_chunks) * 32);
    for (size_t i = 0; i < m.chunk_hashes.size(); ++i) {
        m.chunk_hashes[i] = static_cast<uint8_t>((i * 7) & 0xFF);
    }
    m.filename = "hello.bin";
    return m;
}

} // namespace

TEST_CASE("chunked: Sha3Hasher incremental matches one-shot", "[chunked]") {
    std::vector<uint8_t> a(1024 * 1024, 0x11);
    std::vector<uint8_t> b(1024 * 1024, 0x22);
    std::vector<uint8_t> concat = a;
    concat.insert(concat.end(), b.begin(), b.end());

    Sha3Hasher h;
    h.absorb(a);
    h.absorb(b);
    auto inc = h.finalize();
    auto one = sha3_256(concat);
    REQUIRE(inc == one);
}

TEST_CASE("chunked: Sha3Hasher short-input equivalence", "[chunked]") {
    std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04, 0x05};
    Sha3Hasher h;
    h.absorb(data);
    auto inc = h.finalize();
    auto one = sha3_256(data);
    REQUIRE(inc == one);
}

TEST_CASE("chunked: encode/decode manifest round-trip (2 chunks)", "[chunked]") {
    auto m = make_valid_manifest(2);
    auto bytes = encode_manifest_payload(m);
    REQUIRE(bytes.size() >= 4);
    REQUIRE(bytes[0] == 0x43);
    REQUIRE(bytes[1] == 0x50);
    REQUIRE(bytes[2] == 0x41);
    REQUIRE(bytes[3] == 0x52);

    auto decoded = decode_manifest_payload(bytes);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->version == m.version);
    REQUIRE(decoded->chunk_size_bytes == m.chunk_size_bytes);
    REQUIRE(decoded->segment_count == m.segment_count);
    REQUIRE(decoded->total_plaintext_bytes == m.total_plaintext_bytes);
    REQUIRE(decoded->plaintext_sha3 == m.plaintext_sha3);
    REQUIRE(decoded->chunk_hashes == m.chunk_hashes);
    REQUIRE(decoded->filename == m.filename);
}

TEST_CASE("chunked: encode/decode round-trip 64 chunks", "[chunked]") {
    auto m = make_valid_manifest(64);
    auto bytes = encode_manifest_payload(m);
    auto decoded = decode_manifest_payload(bytes);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->segment_count == 64);
    REQUIRE(decoded->chunk_hashes.size() == 64u * 32u);
    REQUIRE(decoded->chunk_hashes == m.chunk_hashes);
}

TEST_CASE("chunked: decode rejects missing CPAR magic", "[chunked]") {
    auto bytes = encode_manifest_payload(make_valid_manifest(2));
    bytes[0] = 0x00;  // corrupt magic
    REQUIRE_FALSE(decode_manifest_payload(bytes).has_value());
}

TEST_CASE("chunked: decode rejects 0-chunk manifest", "[chunked]") {
    auto m = make_valid_manifest(2);
    m.segment_count = 0;
    m.chunk_hashes.clear();
    m.total_plaintext_bytes = 0;
    auto bytes = encode_manifest_payload(m);
    REQUIRE_FALSE(decode_manifest_payload(bytes).has_value());
}

TEST_CASE("chunked: decode rejects oversized segment_count", "[chunked]") {
    auto m = make_valid_manifest(2);
    m.segment_count = MAX_CHUNKS + 1;
    // leave chunk_hashes at 64 bytes; size mismatch with segment_count rejects
    auto bytes = encode_manifest_payload(m);
    REQUIRE_FALSE(decode_manifest_payload(bytes).has_value());
}

TEST_CASE("chunked: decode rejects truncated chunk_hashes (not multiple of 32)", "[chunked]") {
    auto m = make_valid_manifest(2);
    m.chunk_hashes.pop_back();  // 63 bytes, not 64
    auto bytes = encode_manifest_payload(m);
    REQUIRE_FALSE(decode_manifest_payload(bytes).has_value());
}

TEST_CASE("chunked: decode rejects chunk_size_bytes out of range", "[chunked]") {
    {
        auto m = make_valid_manifest(2);
        m.chunk_size_bytes = 1024;  // below 1 MiB min
        auto bytes = encode_manifest_payload(m);
        REQUIRE_FALSE(decode_manifest_payload(bytes).has_value());
    }
    {
        auto m = make_valid_manifest(2);
        m.chunk_size_bytes = 1024u * 1024u * 1024u;  // 1 GiB, above 256 MiB max
        auto bytes = encode_manifest_payload(m);
        REQUIRE_FALSE(decode_manifest_payload(bytes).has_value());
    }
}

TEST_CASE("chunked: decode rejects version != 1", "[chunked]") {
    auto m = make_valid_manifest(2);
    m.version = 2;
    auto bytes = encode_manifest_payload(m);
    REQUIRE_FALSE(decode_manifest_payload(bytes).has_value());
}

TEST_CASE("chunked: decode rejects total_plaintext_bytes > segment_count*chunk_size", "[chunked]") {
    auto m = make_valid_manifest(2);
    m.total_plaintext_bytes =
        static_cast<uint64_t>(m.segment_count) * m.chunk_size_bytes + 1;
    auto bytes = encode_manifest_payload(m);
    REQUIRE_FALSE(decode_manifest_payload(bytes).has_value());
}

TEST_CASE("chunked: decode rejects plaintext_sha3 wrong size (len!=32 at encode)",
          "[chunked]") {
    // Hand-roll a FlatBuffer with a 31-byte plaintext_sha3 vector, prepend CPAR.
    flatbuffers::FlatBufferBuilder builder(256);
    builder.ForceDefaults(true);
    std::array<uint8_t, 31> bad_sha3{};
    auto sha3 = builder.CreateVector(bad_sha3.data(), bad_sha3.size());
    std::vector<uint8_t> ch(2 * 32, 0x00);
    auto hashes = builder.CreateVector(ch.data(), ch.size());
    auto fname  = builder.CreateString("x");
    auto start = builder.StartTable();
    builder.AddElement<uint32_t>(4,  MANIFEST_VERSION_V1, 0);
    builder.AddElement<uint32_t>(6,  CHUNK_SIZE_BYTES_DEFAULT, 0);
    builder.AddElement<uint32_t>(8,  2, 0);
    builder.AddElement<uint64_t>(10, 1024, 0);
    builder.AddOffset(12, sha3);
    builder.AddOffset(14, hashes);
    builder.AddOffset(16, fname);
    auto root = builder.EndTable(start);
    builder.Finish(flatbuffers::Offset<flatbuffers::Table>(root));
    std::vector<uint8_t> bytes;
    bytes.insert(bytes.end(), CPAR_MAGIC.begin(), CPAR_MAGIC.end());
    bytes.insert(bytes.end(), builder.GetBufferPointer(),
                 builder.GetBufferPointer() + builder.GetSize());
    REQUIRE_FALSE(decode_manifest_payload(bytes).has_value());
}

TEST_CASE("chunked: type_label returns CPAR and is_hidden_type returns false for CPAR",
          "[chunked]") {
    const uint8_t cpar[4] = {0x43, 0x50, 0x41, 0x52};
    REQUIRE(std::string(type_label(cpar)) == "CPAR");
    REQUIRE_FALSE(is_hidden_type(cpar));
}

TEST_CASE("chunked: type_label returns CDAT and is_hidden_type returns true for CDAT",
          "[chunked]") {
    const uint8_t cdat[4] = {0x43, 0x44, 0x41, 0x54};
    REQUIRE(std::string(type_label(cdat)) == "CDAT");
    REQUIRE(is_hidden_type(cdat));
}
