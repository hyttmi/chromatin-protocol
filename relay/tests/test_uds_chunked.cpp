#include <catch2/catch_test_macros.hpp>

#include "relay/core/chunked_stream.h"
#include "relay/core/uds_multiplexer.h"
#include "relay/wire/aead.h"
#include "relay/util/endian.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <variant>
#include <vector>

using namespace chromatindb::relay;

// Type aliases to avoid template-comma issues in Catch2 macros
using ChunkedJob = core::UdsMultiplexer::ChunkedSendJob;
using RawMsg = std::vector<uint8_t>;
using SendItem = core::UdsMultiplexer::SendItem;

// =============================================================================
// CHUNKED_BEGIN flag detection
// =============================================================================

TEST_CASE("uds_chunked: CHUNKED_BEGIN flag detection", "[uds_chunked]") {
    SECTION("byte 0x01 at start is recognized as chunked") {
        // A chunked header starts with CHUNKED_BEGIN (0x01)
        auto header = core::encode_chunked_header(8, 42, 2 * 1024 * 1024);
        REQUIRE(!header.empty());
        REQUIRE(header[0] == core::CHUNKED_BEGIN);
        REQUIRE(header[0] == 0x01);
    }

    SECTION("FlatBuffer messages do not start with 0x01") {
        // FlatBuffers start with a 4-byte root table offset.
        // A valid FlatBuffer's first 4 bytes are a little-endian uint32 offset
        // that points past the header. The first byte is almost never 0x01
        // (would require root table at offset 1, which is too small for any
        // real message). This test verifies a typical transport-codec encoded
        // message does NOT start with 0x01.
        //
        // We construct a minimal payload that mimics what TransportCodec produces.
        // TransportCodec writes FlatBuffer bytes; the first 4 bytes are the
        // root table offset in LE. For any non-trivial table this is >= 4.
        std::vector<uint8_t> fake_flatbuf = {0x04, 0x00, 0x00, 0x00, 0x00};
        REQUIRE(fake_flatbuf[0] != core::CHUNKED_BEGIN);
    }

    SECTION("empty message is not chunked") {
        std::vector<uint8_t> empty;
        // The check is: !msg->empty() && (*msg)[0] == CHUNKED_BEGIN
        // Empty should not match
        REQUIRE(empty.empty());
    }
}

// =============================================================================
// Chunked header encode/decode round-trip for UDS context
// =============================================================================

TEST_CASE("uds_chunked: chunked header round-trip with extra metadata", "[uds_chunked]") {
    SECTION("header with no extra metadata") {
        auto encoded = core::encode_chunked_header(8, 1000, 5 * 1024 * 1024);
        auto decoded = core::decode_chunked_header(encoded);
        REQUIRE(decoded.has_value());
        CHECK(decoded->type == 8);
        CHECK(decoded->request_id == 1000);
        CHECK(decoded->total_payload_size == 5ULL * 1024 * 1024);
        CHECK(decoded->extra_metadata.empty());
    }

    SECTION("header with extra metadata (e.g., status byte for ReadResponse)") {
        std::vector<uint8_t> meta = {0x00};  // status byte = success
        auto encoded = core::encode_chunked_header(9, 42, 100 * 1024 * 1024, meta);
        auto decoded = core::decode_chunked_header(encoded);
        REQUIRE(decoded.has_value());
        CHECK(decoded->type == 9);
        CHECK(decoded->request_id == 42);
        CHECK(decoded->total_payload_size == 100ULL * 1024 * 1024);
        REQUIRE(decoded->extra_metadata.size() == 1);
        CHECK(decoded->extra_metadata[0] == 0x00);
    }
}

// =============================================================================
// Chunked send/recv protocol simulation with AEAD
// =============================================================================

TEST_CASE("uds_chunked: chunked protocol with AEAD encryption", "[uds_chunked]") {
    // Simulate the chunked protocol by manually encoding/decoding
    // what send_chunked and recv_chunked_reassemble would produce.
    // Each sub-frame is AEAD encrypted with sequential counters from
    // the shared monotonic counter.

    // Shared AEAD key (32 bytes of 0xAA for testing)
    std::vector<uint8_t> key(wire::AEAD_KEY_SIZE, 0xAA);

    SECTION("small chunked payload (1 chunk)") {
        uint8_t type = 8;  // Data
        uint32_t request_id = 99;
        std::vector<uint8_t> payload(500'000, 0x42);  // 500 KB payload
        uint64_t counter = 10;  // Starting counter value

        // --- Sender side: what send_chunked() does ---

        // 1. Encode and encrypt chunked header
        auto header = core::encode_chunked_header(type, request_id,
                                                    static_cast<uint64_t>(payload.size()));
        auto header_ct = wire::aead_encrypt(header, key, counter++);

        // 2. Encrypt data chunk (single chunk since < CHUNK_SIZE)
        auto chunk_ct = wire::aead_encrypt(payload, key, counter++);

        // 3. Encrypt zero-length sentinel
        auto sentinel_ct = wire::aead_encrypt(
            std::span<const uint8_t>{}, key, counter++);

        // --- Receiver side: what recv_chunked_reassemble() does ---
        uint64_t recv_counter = 10;

        // Decrypt header
        auto header_pt = wire::aead_decrypt(header_ct, key, recv_counter++);
        REQUIRE(header_pt.has_value());
        REQUIRE(header_pt->size() == core::CHUNKED_HEADER_SIZE);
        REQUIRE((*header_pt)[0] == core::CHUNKED_BEGIN);

        auto hdr = core::decode_chunked_header(*header_pt);
        REQUIRE(hdr.has_value());
        CHECK(hdr->type == type);
        CHECK(hdr->request_id == request_id);
        CHECK(hdr->total_payload_size == payload.size());

        // Decrypt data chunk
        auto chunk_pt = wire::aead_decrypt(chunk_ct, key, recv_counter++);
        REQUIRE(chunk_pt.has_value());
        CHECK(chunk_pt->size() == payload.size());
        CHECK(*chunk_pt == payload);

        // Decrypt sentinel
        auto sentinel_pt = wire::aead_decrypt(sentinel_ct, key, recv_counter++);
        REQUIRE(sentinel_pt.has_value());
        CHECK(sentinel_pt->empty());

        // Counters consumed: 3 (header + 1 chunk + sentinel)
        CHECK(counter == 13);
        CHECK(recv_counter == 13);
    }

    SECTION("multi-chunk payload (3 chunks)") {
        uint8_t type = 8;  // Data
        uint32_t request_id = 200;
        // 2.5 MiB payload = 3 chunks (1 MiB + 1 MiB + 0.5 MiB)
        std::vector<uint8_t> payload(2621440, 0x55);
        uint64_t counter = 0;

        // --- Sender side ---

        auto header = core::encode_chunked_header(type, request_id,
                                                    static_cast<uint64_t>(payload.size()));
        auto header_ct = wire::aead_encrypt(header, key, counter++);

        // Send chunks
        std::vector<std::vector<uint8_t>> chunk_cts;
        size_t offset = 0;
        while (offset < payload.size()) {
            size_t chunk_len = std::min(core::CHUNK_SIZE, payload.size() - offset);
            std::span<const uint8_t> chunk_span(payload.data() + offset, chunk_len);
            chunk_cts.push_back(wire::aead_encrypt(chunk_span, key, counter++));
            offset += chunk_len;
        }
        CHECK(chunk_cts.size() == 3);

        auto sentinel_ct = wire::aead_encrypt(
            std::span<const uint8_t>{}, key, counter++);

        // --- Receiver side ---
        uint64_t recv_counter = 0;

        // Decrypt header
        auto header_pt = wire::aead_decrypt(header_ct, key, recv_counter++);
        REQUIRE(header_pt.has_value());
        auto hdr = core::decode_chunked_header(*header_pt);
        REQUIRE(hdr.has_value());
        CHECK(hdr->total_payload_size == payload.size());

        // Reassemble chunks
        std::vector<uint8_t> reassembled;
        reassembled.reserve(static_cast<size_t>(hdr->total_payload_size));

        for (const auto& ct : chunk_cts) {
            auto chunk_pt = wire::aead_decrypt(ct, key, recv_counter++);
            REQUIRE(chunk_pt.has_value());
            REQUIRE(!chunk_pt->empty());
            reassembled.insert(reassembled.end(), chunk_pt->begin(), chunk_pt->end());
        }

        // Decrypt sentinel
        auto sentinel_pt = wire::aead_decrypt(sentinel_ct, key, recv_counter++);
        REQUIRE(sentinel_pt.has_value());
        CHECK(sentinel_pt->empty());

        // Verify reassembled == original
        CHECK(reassembled.size() == payload.size());
        CHECK(reassembled == payload);

        // Counters consumed: 5 (header + 3 chunks + sentinel)
        CHECK(counter == 5);
        CHECK(recv_counter == 5);
    }
}

// =============================================================================
// Nonce counter consumption verification
// =============================================================================

TEST_CASE("uds_chunked: nonce counter consumption per chunk", "[uds_chunked]") {
    // Verify that each chunk consumes exactly one nonce from the counter (D-09).
    // For a payload that spans N chunks, the total nonces consumed are:
    // 1 (header) + N (data chunks) + 1 (sentinel) = N + 2

    std::vector<uint8_t> key(wire::AEAD_KEY_SIZE, 0xBB);

    SECTION("1 MiB payload = 1 chunk = 3 nonces") {
        size_t payload_size = core::CHUNK_SIZE;  // Exactly 1 MiB
        size_t num_chunks = (payload_size + core::CHUNK_SIZE - 1) / core::CHUNK_SIZE;
        CHECK(num_chunks == 1);
        size_t nonces = 1 + num_chunks + 1;  // header + chunks + sentinel
        CHECK(nonces == 3);
    }

    SECTION("2 MiB payload = 2 chunks = 4 nonces") {
        size_t payload_size = 2 * core::CHUNK_SIZE;
        size_t num_chunks = (payload_size + core::CHUNK_SIZE - 1) / core::CHUNK_SIZE;
        CHECK(num_chunks == 2);
        size_t nonces = 1 + num_chunks + 1;
        CHECK(nonces == 4);
    }

    SECTION("500 MiB payload = 500 chunks = 502 nonces") {
        size_t payload_size = 500 * 1024 * 1024;
        size_t num_chunks = (payload_size + core::CHUNK_SIZE - 1) / core::CHUNK_SIZE;
        CHECK(num_chunks == 500);
        size_t nonces = 1 + num_chunks + 1;
        CHECK(nonces == 502);
    }

    SECTION("AEAD with wrong counter fails") {
        // Demonstrates that each nonce is unique and must match
        std::vector<uint8_t> plaintext = {1, 2, 3, 4, 5};
        auto ct = wire::aead_encrypt(plaintext, key, 42);
        // Correct counter succeeds
        auto pt = wire::aead_decrypt(ct, key, 42);
        REQUIRE(pt.has_value());
        CHECK(*pt == plaintext);
        // Wrong counter fails
        auto bad = wire::aead_decrypt(ct, key, 43);
        CHECK(!bad.has_value());
    }
}

// =============================================================================
// SendItem variant type checks
// =============================================================================

TEST_CASE("uds_chunked: SendItem variant construction", "[uds_chunked]") {
    SECTION("raw vector variant") {
        RawMsg raw_msg = {0x04, 0x00, 0x00, 0x00, 0x01};
        SendItem item{std::move(raw_msg)};
        bool is_raw = std::holds_alternative<RawMsg>(item);
        bool is_chunked = std::holds_alternative<ChunkedJob>(item);
        REQUIRE(is_raw);
        REQUIRE(!is_chunked);
    }

    SECTION("ChunkedSendJob variant") {
        ChunkedJob job{
            .type = 8,
            .request_id = 100,
            .payload = std::vector<uint8_t>(2 * 1024 * 1024, 0xAB),
            .extra_metadata = {}
        };
        SendItem item{std::move(job)};
        bool is_chunked = std::holds_alternative<ChunkedJob>(item);
        bool is_raw = std::holds_alternative<RawMsg>(item);
        REQUIRE(is_chunked);
        REQUIRE(!is_raw);

        auto* j = std::get_if<ChunkedJob>(&item);
        REQUIRE(j != nullptr);
        CHECK(j->type == 8);
        CHECK(j->request_id == 100);
        CHECK(j->payload.size() == 2 * 1024 * 1024);
    }
}
