#include <catch2/catch_test_macros.hpp>
#include "cli/src/chunked.h"
#include "cli/src/envelope.h"
#include "cli/src/identity.h"
#include "cli/src/wire.h"

#include <flatbuffers/flatbuffers.h>

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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

// =============================================================================
// Task 2 tests — chunked.h free functions + outer-magic placement
// =============================================================================

TEST_CASE("chunked: outer-magic placement on blob.data (CDAT)", "[chunked]") {
    // D-13: blob.data = [CDAT magic:4][CENV envelope(raw plaintext)].
    // Test without network: build an envelope locally, prepend CDAT, verify
    // the outer layout is exactly what the planner locked in.
    auto id = Identity::generate();
    std::vector<std::span<const uint8_t>> recipients;
    auto kem_pk = id.kem_pubkey();
    recipients.emplace_back(kem_pk);

    std::vector<uint8_t> plaintext(32, 0xAB);
    auto cenv = envelope::encrypt(plaintext, recipients);
    REQUIRE(cenv.size() >= 4);
    REQUIRE(envelope::is_envelope(cenv));

    std::vector<uint8_t> blob_data;
    blob_data.reserve(4 + cenv.size());
    blob_data.insert(blob_data.end(), CDAT_MAGIC.begin(), CDAT_MAGIC.end());
    blob_data.insert(blob_data.end(), cenv.begin(), cenv.end());

    // Outer magic is CDAT, not CENV — Phase 117 type index sees a chunk.
    REQUIRE(blob_data.size() >= 4);
    REQUIRE(std::memcmp(blob_data.data(), CDAT_MAGIC.data(), 4) == 0);
    // Bytes [4..) are the CENV envelope.
    auto inner = std::span<const uint8_t>(blob_data.data() + 4,
                                          blob_data.size() - 4);
    REQUIRE(envelope::is_envelope(inner));
}

TEST_CASE("chunked: read_next_chunk reads full then short then EOF", "[chunked]") {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() /
               ("cdb_chunked_test_" + std::to_string(::getpid()) + ".bin");
    struct Guard {
        fs::path p;
        ~Guard() { std::error_code ec; fs::remove(p, ec); }
    } guard{tmp};

    // Write 45 bytes with a 16-byte "chunk" size so reads land at 16/16/13.
    constexpr size_t CHUNK = 16;
    constexpr size_t TOTAL = 45;
    {
        std::ofstream out(tmp, std::ios::binary);
        REQUIRE(out);
        std::vector<uint8_t> data(TOTAL);
        for (size_t i = 0; i < TOTAL; ++i) data[i] = static_cast<uint8_t>(i);
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(TOTAL));
    }

    std::ifstream f(tmp, std::ios::binary);
    REQUIRE(f);
    std::vector<uint8_t> buf;
    REQUIRE(chunked::read_next_chunk(f, buf, CHUNK) == CHUNK);
    REQUIRE(chunked::read_next_chunk(f, buf, CHUNK) == CHUNK);
    REQUIRE(chunked::read_next_chunk(f, buf, CHUNK) == 13);
    REQUIRE(chunked::read_next_chunk(f, buf, CHUNK) == 0);
}

TEST_CASE("chunked: plan_tombstone_targets orders chunks first, manifest last",
          "[chunked]") {
    ManifestData m;
    m.version = MANIFEST_VERSION_V1;
    m.chunk_size_bytes = CHUNK_SIZE_BYTES_DEFAULT;
    m.segment_count = 3;
    m.total_plaintext_bytes = 1024;
    for (size_t i = 0; i < 32; ++i) m.plaintext_sha3[i] = 0;
    m.chunk_hashes.resize(3 * 32);
    // chunk 0 hash is all 0x10, chunk 1 is 0x11, chunk 2 is 0x12
    for (size_t i = 0; i < 32; ++i) m.chunk_hashes[0 * 32 + i]  = 0x10;
    for (size_t i = 0; i < 32; ++i) m.chunk_hashes[1 * 32 + i]  = 0x11;
    for (size_t i = 0; i < 32; ++i) m.chunk_hashes[2 * 32 + i]  = 0x12;
    std::array<uint8_t, 32> manifest_hash{};
    for (size_t i = 0; i < 32; ++i) manifest_hash[i] = 0x99;

    auto plan = chunked::plan_tombstone_targets(m,
        std::span<const uint8_t, 32>(manifest_hash.data(), 32));

    REQUIRE(plan.size() == 4);
    // chunk 0 first
    for (size_t i = 0; i < 32; ++i) REQUIRE(plan[0][i] == 0x10);
    for (size_t i = 0; i < 32; ++i) REQUIRE(plan[1][i] == 0x11);
    for (size_t i = 0; i < 32; ++i) REQUIRE(plan[2][i] == 0x12);
    // manifest last
    for (size_t i = 0; i < 32; ++i) REQUIRE(plan[3][i] == 0x99);
}

// =============================================================================
// Plan 02 Task 1 — get_chunked pure helpers (plan_chunk_read_targets,
// verify_plaintext_sha3, refuse_if_exists) and out-of-order pwrite semantics
// =============================================================================

TEST_CASE("chunked: plan_chunk_read_targets preserves chunk_index order", "[chunked]") {
    auto m = make_valid_manifest(3);
    // set hashes to distinctive patterns so ordering is observable:
    // chunk 0 => 0x00 XX, chunk 1 => 0x10 XX, chunk 2 => 0x20 XX
    for (size_t i = 0; i < m.chunk_hashes.size(); ++i) {
        m.chunk_hashes[i] = static_cast<uint8_t>((i / 32) * 0x10 + (i & 0x0F));
    }
    auto targets = chunked::plan_chunk_read_targets(m);
    REQUIRE(targets.size() == 3);
    // chunk 0's first byte = 0x00, chunk 1's first byte = 0x10, chunk 2's = 0x20
    REQUIRE(targets[0][0] == 0x00);
    REQUIRE(targets[1][0] == 0x10);
    REQUIRE(targets[2][0] == 0x20);
}

TEST_CASE("chunked: verify_plaintext_sha3 matches when file hashes match", "[chunked]") {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() /
               ("chunked_verify_ok_" + std::to_string(::getpid()));
    struct Guard {
        fs::path p;
        ~Guard() { std::error_code ec; fs::remove(p, ec); }
    } guard{tmp};

    std::vector<uint8_t> data(32 * 1024 * 1024, 0xAB);   // 32 MiB
    {
        std::ofstream f(tmp, std::ios::binary);
        REQUIRE(f);
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
    }
    auto expected = sha3_256(data);
    REQUIRE(chunked::verify_plaintext_sha3(
        tmp.string(),
        std::span<const uint8_t, 32>(expected.data(), 32)));
}

TEST_CASE("chunked: verify_plaintext_sha3 rejects wrong hash", "[chunked]") {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() /
               ("chunked_verify_bad_" + std::to_string(::getpid()));
    struct Guard {
        fs::path p;
        ~Guard() { std::error_code ec; fs::remove(p, ec); }
    } guard{tmp};

    std::vector<uint8_t> data(1024, 0xCD);
    {
        std::ofstream f(tmp, std::ios::binary);
        REQUIRE(f);
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
    }
    std::array<uint8_t, 32> wrong{};
    wrong[0] = 0xFF;
    REQUIRE_FALSE(chunked::verify_plaintext_sha3(
        tmp.string(),
        std::span<const uint8_t, 32>(wrong.data(), 32)));
}

TEST_CASE("chunked: verify_plaintext_sha3 false on missing file", "[chunked]") {
    std::array<uint8_t, 32> any{};
    REQUIRE_FALSE(chunked::verify_plaintext_sha3(
        "/tmp/chunked_nonexistent_path_xyz_plan02_task1",
        std::span<const uint8_t, 32>(any.data(), 32)));
}

TEST_CASE("chunked: pwrite out-of-order lands chunks at correct offsets", "[chunked]") {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() /
               ("chunked_pwrite_test_" + std::to_string(::getpid()));
    struct Guard {
        fs::path p;
        ~Guard() { std::error_code ec; fs::remove(p, ec); }
    } guard{tmp};
    std::error_code ec; fs::remove(tmp, ec);

    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    REQUIRE(fd >= 0);
    const size_t chunk_size = 64 * 1024;
    const uint32_t N = 3;
    REQUIRE(::ftruncate(fd, static_cast<off_t>(N * chunk_size)) == 0);

    std::vector<uint8_t> c0(chunk_size, 0xA1);
    std::vector<uint8_t> c1(chunk_size, 0xB2);
    std::vector<uint8_t> c2(chunk_size, 0xC3);

    // Write out-of-order: 2, 0, 1.
    REQUIRE(::pwrite(fd, c2.data(), chunk_size, 2 * chunk_size)
            == static_cast<ssize_t>(chunk_size));
    REQUIRE(::pwrite(fd, c0.data(), chunk_size, 0 * chunk_size)
            == static_cast<ssize_t>(chunk_size));
    REQUIRE(::pwrite(fd, c1.data(), chunk_size, 1 * chunk_size)
            == static_cast<ssize_t>(chunk_size));
    ::close(fd);

    std::ifstream f(tmp, std::ios::binary);
    REQUIRE(f);
    std::vector<uint8_t> buf(N * chunk_size);
    f.read(reinterpret_cast<char*>(buf.data()),
           static_cast<std::streamsize>(buf.size()));
    REQUIRE(f.gcount() == static_cast<std::streamsize>(N * chunk_size));

    for (size_t i = 0; i < chunk_size; ++i) REQUIRE(buf[i] == 0xA1);
    for (size_t i = 0; i < chunk_size; ++i) REQUIRE(buf[chunk_size + i] == 0xB2);
    for (size_t i = 0; i < chunk_size; ++i) REQUIRE(buf[2 * chunk_size + i] == 0xC3);
}

TEST_CASE("chunked: refuse_if_exists blocks pre-existing file without --force",
          "[chunked]") {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() /
               ("chunked_overwrite_guard_" + std::to_string(::getpid()));
    struct Guard {
        fs::path p;
        ~Guard() { std::error_code ec; fs::remove(p, ec); }
    } guard{tmp};
    std::error_code ec; fs::remove(tmp, ec);

    {
        std::ofstream f(tmp);
        REQUIRE(f);
        f << "pre-existing";
    }
    REQUIRE(fs::exists(tmp));
    REQUIRE_FALSE(chunked::refuse_if_exists(tmp.string(), /*force=*/false));
    // Helper MUST NOT delete the file: it is a pure existence probe.
    REQUIRE(fs::exists(tmp));
}

TEST_CASE("chunked: refuse_if_exists permits overwrite with --force", "[chunked]") {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() /
               ("chunked_overwrite_guard_force_" + std::to_string(::getpid()));
    struct Guard {
        fs::path p;
        ~Guard() { std::error_code ec; fs::remove(p, ec); }
    } guard{tmp};
    std::error_code ec; fs::remove(tmp, ec);

    {
        std::ofstream f(tmp);
        REQUIRE(f);
        f << "pre-existing";
    }
    REQUIRE(chunked::refuse_if_exists(tmp.string(), /*force=*/true));
}

TEST_CASE("chunked: refuse_if_exists permits fresh path in both modes", "[chunked]") {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() /
               ("chunked_overwrite_guard_fresh_" + std::to_string(::getpid()));
    std::error_code ec; fs::remove(tmp, ec);   // ensure absent
    REQUIRE(chunked::refuse_if_exists(tmp.string(), /*force=*/false));
    REQUIRE(chunked::refuse_if_exists(tmp.string(), /*force=*/true));
}
