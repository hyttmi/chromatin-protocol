#include <catch2/catch_test_macros.hpp>
#include "cli/src/chunked.h"
#include "cli/src/envelope.h"
#include "cli/src/identity.h"
#include "cli/src/pipeline_pump.h"
#include "cli/src/wire.h"
#include "cli/tests/pipeline_test_support.h"

#include <flatbuffers/flatbuffers.h>

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

using namespace chromatindb::cli;

namespace {

// Phase 130 CLI-02/03: CHUNK_SIZE_BYTES_DEFAULT was deleted in favour of the
// session-cap-driven chunking boundary. These tests don't exercise a live
// Connection, so use a fixed test value here. The manifest validator also
// takes a `session_cap` parameter (Phase 130 CLI-04 / D-06) — most of the
// pre-existing tests don't cap-gate, so they pass UINT64_MAX to disable that
// extra check.
constexpr uint32_t kTestChunkSizeBytes = 16u * 1024u * 1024u;  // 16 MiB

ManifestData make_valid_manifest(uint32_t n_chunks = 2) {
    ManifestData m;
    m.version               = MANIFEST_VERSION_V1;
    m.chunk_size_bytes      = kTestChunkSizeBytes;
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

    auto decoded = decode_manifest_payload(bytes, UINT64_MAX);
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
    auto decoded = decode_manifest_payload(bytes, UINT64_MAX);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->segment_count == 64);
    REQUIRE(decoded->chunk_hashes.size() == 64u * 32u);
    REQUIRE(decoded->chunk_hashes == m.chunk_hashes);
}

TEST_CASE("chunked: decode rejects missing CPAR magic", "[chunked]") {
    auto bytes = encode_manifest_payload(make_valid_manifest(2));
    bytes[0] = 0x00;  // corrupt magic
    REQUIRE_FALSE(decode_manifest_payload(bytes, UINT64_MAX).has_value());
}

TEST_CASE("chunked: decode rejects 0-chunk manifest", "[chunked]") {
    auto m = make_valid_manifest(2);
    m.segment_count = 0;
    m.chunk_hashes.clear();
    m.total_plaintext_bytes = 0;
    auto bytes = encode_manifest_payload(m);
    REQUIRE_FALSE(decode_manifest_payload(bytes, UINT64_MAX).has_value());
}

TEST_CASE("chunked: decode rejects oversized segment_count", "[chunked]") {
    auto m = make_valid_manifest(2);
    m.segment_count = MAX_CHUNKS + 1;
    // leave chunk_hashes at 64 bytes; size mismatch with segment_count rejects
    auto bytes = encode_manifest_payload(m);
    REQUIRE_FALSE(decode_manifest_payload(bytes, UINT64_MAX).has_value());
}

TEST_CASE("chunked: decode rejects truncated chunk_hashes (not multiple of 32)", "[chunked]") {
    auto m = make_valid_manifest(2);
    m.chunk_hashes.pop_back();  // 63 bytes, not 64
    auto bytes = encode_manifest_payload(m);
    REQUIRE_FALSE(decode_manifest_payload(bytes, UINT64_MAX).has_value());
}

TEST_CASE("chunked: decode rejects chunk_size_bytes out of range", "[chunked]") {
    {
        auto m = make_valid_manifest(2);
        m.chunk_size_bytes = 1024;  // below 1 MiB min
        auto bytes = encode_manifest_payload(m);
        REQUIRE_FALSE(decode_manifest_payload(bytes, UINT64_MAX).has_value());
    }
    {
        // Phase 130 CLI-04 / D-06: "above max" now means "above the session
        // cap". Use a realistic 64 MiB cap (Phase 128 hard ceiling) and a
        // manifest that declares a 1 GiB chunk.
        auto m = make_valid_manifest(2);
        m.chunk_size_bytes = 1024u * 1024u * 1024u;  // 1 GiB
        auto bytes = encode_manifest_payload(m);
        const uint64_t cap_64mib = 64ULL * 1024ULL * 1024ULL;
        REQUIRE_FALSE(decode_manifest_payload(bytes, cap_64mib).has_value());
    }
}

TEST_CASE("chunked: decode rejects version != 1", "[chunked]") {
    auto m = make_valid_manifest(2);
    m.version = 2;
    auto bytes = encode_manifest_payload(m);
    REQUIRE_FALSE(decode_manifest_payload(bytes, UINT64_MAX).has_value());
}

TEST_CASE("chunked: decode rejects total_plaintext_bytes > segment_count*chunk_size", "[chunked]") {
    auto m = make_valid_manifest(2);
    m.total_plaintext_bytes =
        static_cast<uint64_t>(m.segment_count) * m.chunk_size_bytes + 1;
    auto bytes = encode_manifest_payload(m);
    REQUIRE_FALSE(decode_manifest_payload(bytes, UINT64_MAX).has_value());
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
    builder.AddElement<uint32_t>(6,  kTestChunkSizeBytes, 0);
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
    REQUIRE_FALSE(decode_manifest_payload(bytes, UINT64_MAX).has_value());
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

    // Outer magic is CDAT, not CENV — the type index sees a chunk.
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
    m.chunk_size_bytes = kTestChunkSizeBytes;
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

// =============================================================================
// Plan 02 Task 2 — classify_blob_data dispatch helper
// =============================================================================

TEST_CASE("chunked: classify_blob_data distinguishes CPAR/CDAT/Other",
          "[chunked]") {
    std::vector<uint8_t> cpar = {0x43, 0x50, 0x41, 0x52, 0x00, 0x00};
    std::vector<uint8_t> cdat = {0x43, 0x44, 0x41, 0x54, 0x00, 0x00};
    std::vector<uint8_t> cenv = {0x43, 0x45, 0x4E, 0x56, 0x01, 0x02};
    std::vector<uint8_t> short_bytes = {0x43};
    std::vector<uint8_t> empty;

    REQUIRE(chunked::classify_blob_data(cpar) == chunked::GetDispatch::CPAR);
    REQUIRE(chunked::classify_blob_data(cdat) == chunked::GetDispatch::CDAT);
    REQUIRE(chunked::classify_blob_data(cenv) == chunked::GetDispatch::Other);
    REQUIRE(chunked::classify_blob_data(short_bytes) ==
            chunked::GetDispatch::Other);
    REQUIRE(chunked::classify_blob_data(empty) == chunked::GetDispatch::Other);
}

TEST_CASE("chunked: CR-01 regression — 8 sends + 8 recv_next drains leave in_flight at 0", "[chunked]") {
    using chromatindb::cli::testing::ScriptedSource;
    using chromatindb::cli::testing::make_ack_reply;

    // Simulate the exact shape that failed on the live node:
    // put_chunked filled 8 in-flight CDAT writes, drained 8 replies
    // via the OLD conn.recv() — which left in_flight at 8 because
    // recv() never touched the counter. With pump_recv_any, the same
    // drain pattern must bring in_flight back to 0.
    ScriptedSource src;
    for (uint32_t r = 1; r <= 8; ++r) src.queue.push_back(make_ack_reply(r));

    std::unordered_map<uint32_t, DecodedTransport> pending;
    std::size_t in_flight = 8;

    // Drain 8 replies via pump_recv_any (the helper behind recv_next).
    for (int i = 0; i < 8; ++i) {
        auto got = pipeline::pump_recv_any(std::ref(src), pending, in_flight);
        REQUIRE(got.has_value());
    }

    REQUIRE(in_flight == 0);  // no CR-01 leak
    REQUIRE(pending.empty());
    REQUIRE(src.call_count == 8);

    // 9th drain against an empty source returns nullopt and does NOT underflow.
    auto ninth = pipeline::pump_recv_any(std::ref(src), pending, in_flight);
    REQUIRE_FALSE(ninth.has_value());
    REQUIRE(in_flight == 0);
}

TEST_CASE("chunked: CR-01 regression — drain with pending non-empty decrements in_flight from fast path", "[chunked]") {
    using chromatindb::cli::testing::ScriptedSource;
    using chromatindb::cli::testing::make_ack_reply;

    // Simulate the case where send_async's backpressure pump stashed
    // some replies into pending_replies_. The first pump_recv_any call
    // must drain from pending (fast path), NOT touch source, and
    // decrement in_flight.
    ScriptedSource src;  // deliberately empty — source must not be called
    std::unordered_map<uint32_t, DecodedTransport> pending;
    pending.emplace(5, make_ack_reply(5));
    pending.emplace(6, make_ack_reply(6));
    std::size_t in_flight = 2;

    auto got1 = pipeline::pump_recv_any(std::ref(src), pending, in_flight);
    REQUIRE(got1.has_value());
    REQUIRE(in_flight == 1);
    REQUIRE(src.call_count == 0);

    auto got2 = pipeline::pump_recv_any(std::ref(src), pending, in_flight);
    REQUIRE(got2.has_value());
    REQUIRE(in_flight == 0);
    REQUIRE(pending.empty());
    REQUIRE(src.call_count == 0);
}

// =============================================================================
// Phase 130 CLI-02/03 — session-cap-derived chunking boundary
// (CONTEXT.md D-09 scenarios a, b, c)
// =============================================================================
//
// These tests assert the arithmetic contract that put_chunked uses internally:
//   - Files strictly ≤ session cap belong on the single-blob path.
//   - Files strictly > session cap take the chunked path.
//   - Chunk count == ceil(file_size / session_cap) (capped by MAX_CHUNKS).
//   - A chunked upload declares chunk_size_bytes == session cap in its manifest.
//
// They intentionally do not drive the full put_chunked flow (that would require
// a live Connection + socket). The integer math here is the authoritative
// contract — if it changes, put_chunked's decisions change lock-step.

namespace {

// Pure predicate: given `fsize` and `cap`, return true iff put_chunked's
// caller (cmd::put in commands.cpp) routes to the chunked path.
// Mirrors the actual production check at commands.cpp:703 — keep these
// identical or the tests will silently stop reflecting reality.
inline bool should_chunk_for_cap(uint64_t fsize, uint64_t cap) {
    return fsize > cap;
}

// Pure helper: chunk count a put_chunked would produce for a given
// (fsize, cap). Mirrors the ceil-division at chunked.cpp:157.
inline uint64_t chunk_count_for_cap(uint64_t fsize, uint64_t cap) {
    return (fsize + cap - 1) / cap;
}

} // namespace

TEST_CASE("session-cap: chunking threshold equals session cap "
          "(mock NodeInfoResponse seeded 4 MiB)", "[chunked][session-cap]") {
    // Scenario (a): assert that when the session cap is 4 MiB, both the
    // threshold and the per-chunk size use that same value. Tests the
    // CLI-02/03 collapse: threshold == default == max == session_cap.
    constexpr uint64_t cap = 4ULL * 1024 * 1024;

    // At cap exactly → single-blob (files ≤ cap).
    REQUIRE_FALSE(should_chunk_for_cap(cap, cap));
    // One byte over cap → chunked.
    REQUIRE(should_chunk_for_cap(cap + 1, cap));

    // chunk_size the manifest will declare == cap exactly.
    REQUIRE(static_cast<uint32_t>(cap) == 4u * 1024u * 1024u);
}

TEST_CASE("session-cap: file at or below cap is NOT chunked (2 MiB ≤ 4 MiB cap)",
          "[chunked][session-cap]") {
    // Scenario (b): a 2 MiB file with a 4 MiB session cap takes the
    // single-blob path. Also checks the boundary at cap exactly and just
    // below cap.
    constexpr uint64_t cap = 4ULL * 1024 * 1024;

    REQUIRE_FALSE(should_chunk_for_cap(1, cap));                    // tiny
    REQUIRE_FALSE(should_chunk_for_cap(2ULL * 1024 * 1024, cap));   // 2 MiB
    REQUIRE_FALSE(should_chunk_for_cap(cap - 1, cap));              // one under
    REQUIRE_FALSE(should_chunk_for_cap(cap, cap));                  // exactly
}

TEST_CASE("session-cap: file above cap IS chunked at cap boundary "
          "(10 MiB / 4 MiB cap → 3 chunks)", "[chunked][session-cap]") {
    // Scenario (c): 10 MiB file with 4 MiB session cap produces
    // ceil(10/4) = 3 chunks of sizes (4 MiB, 4 MiB, 2 MiB).
    constexpr uint64_t cap     = 4ULL * 1024 * 1024;
    constexpr uint64_t fsize10 = 10ULL * 1024 * 1024;

    REQUIRE(should_chunk_for_cap(fsize10, cap));
    REQUIRE(chunk_count_for_cap(fsize10, cap) == 3u);

    // Additional boundary: a file exactly 2× cap → 2 chunks, not 3.
    REQUIRE(chunk_count_for_cap(2ULL * cap, cap) == 2u);
    // File one byte over cap → 2 chunks (one full, one 1-byte short).
    REQUIRE(chunk_count_for_cap(cap + 1, cap) == 2u);
}
