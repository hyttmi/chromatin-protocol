#pragma once

#include "relay/util/endian.h"

#include <asio.hpp>
#include <asio/as_tuple.hpp>
#include <asio/use_awaitable.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

namespace chromatindb::relay::core {

/// Completion token for non-throwing async operations.
constexpr auto use_nothrow_awaitable = asio::as_tuple(asio::use_awaitable);

// =============================================================================
// Chunked sub-frame protocol constants (per D-02, D-04, D-05, D-06)
// =============================================================================

/// Streaming threshold: payloads >= this use chunked sub-frames (per D-02).
constexpr size_t STREAMING_THRESHOLD = 1048576;  // 1 MiB

/// Chunk size for UDS sub-frames (per D-06).
constexpr size_t CHUNK_SIZE = 1048576;  // 1 MiB

/// Flags byte for chunked sub-frame header.
constexpr uint8_t CHUNKED_BEGIN = 0x01;

/// Maximum depth of backpressure queue (per D-04, 4 chunks = 4 MiB).
constexpr size_t CHUNK_QUEUE_MAX_DEPTH = 4;

/// Chunked header size: [flags:1][type:1][request_id:4BE][total_size:8BE] = 14 bytes.
constexpr size_t CHUNKED_HEADER_SIZE = 14;

// =============================================================================
// Chunked header encode/decode helpers
// =============================================================================

/// Encode a chunked header: [flags:1][type:1][request_id:4BE][total_size:8BE] = 14 bytes.
/// Optional extra_metadata appended after the 14-byte header (e.g., status byte for ReadResponse).
inline std::vector<uint8_t> encode_chunked_header(uint8_t type, uint32_t request_id,
                                                    uint64_t total_payload_size,
                                                    std::span<const uint8_t> extra_metadata = {}) {
    std::vector<uint8_t> header(CHUNKED_HEADER_SIZE + extra_metadata.size());
    header[0] = CHUNKED_BEGIN;
    header[1] = type;
    util::store_u32_be(header.data() + 2, request_id);
    util::store_u64_be(header.data() + 6, total_payload_size);
    if (!extra_metadata.empty()) {
        std::copy(extra_metadata.begin(), extra_metadata.end(),
                  header.begin() + CHUNKED_HEADER_SIZE);
    }
    return header;
}

/// Decoded chunked header.
struct ChunkedHeader {
    uint8_t type;
    uint32_t request_id;
    uint64_t total_payload_size;
    std::vector<uint8_t> extra_metadata;  // bytes after the 14-byte header
};

/// Decode a chunked header from raw bytes. Returns nullopt if too short or flags != CHUNKED_BEGIN.
inline std::optional<ChunkedHeader> decode_chunked_header(std::span<const uint8_t> data) {
    if (data.size() < CHUNKED_HEADER_SIZE) return std::nullopt;
    if (data[0] != CHUNKED_BEGIN) return std::nullopt;

    ChunkedHeader hdr;
    hdr.type = data[1];
    hdr.request_id = util::read_u32_be(data.data() + 2);
    hdr.total_payload_size = util::read_u64_be(data.data() + 6);

    if (data.size() > CHUNKED_HEADER_SIZE) {
        hdr.extra_metadata.assign(data.begin() + CHUNKED_HEADER_SIZE, data.end());
    }

    return hdr;
}

// =============================================================================
// ChunkQueue: backpressure queue for chunked streaming (per D-04)
// =============================================================================

/// Bounded async producer-consumer queue for streaming chunks.
/// Uses asio::steady_timer as a signal mechanism (same pattern as SseWriter and drain_send_queue).
/// Producer pauses when queue is full; consumer waits when queue is empty.
struct ChunkQueue {
    explicit ChunkQueue(asio::any_io_executor executor)
        : signal_(executor) {
        signal_.expires_at(asio::steady_timer::time_point::max());
    }

    std::deque<std::vector<uint8_t>> chunks;
    asio::steady_timer signal_;
    bool closed = false;

    /// Producer: push chunk, co_await if queue full. Returns false if closed.
    asio::awaitable<bool> push(std::vector<uint8_t> chunk) {
        while (chunks.size() >= CHUNK_QUEUE_MAX_DEPTH && !closed) {
            signal_.expires_at(asio::steady_timer::time_point::max());
            auto [ec] = co_await signal_.async_wait(use_nothrow_awaitable);
            if (closed) co_return false;
        }
        if (closed) co_return false;
        chunks.push_back(std::move(chunk));
        signal_.cancel();  // Wake consumer
        co_return true;
    }

    /// Consumer: pop chunk, co_await if empty. Returns nullopt if closed and empty.
    asio::awaitable<std::optional<std::vector<uint8_t>>> pop() {
        while (chunks.empty() && !closed) {
            signal_.expires_at(asio::steady_timer::time_point::max());
            auto [ec] = co_await signal_.async_wait(use_nothrow_awaitable);
        }
        if (chunks.empty()) co_return std::nullopt;
        auto chunk = std::move(chunks.front());
        chunks.pop_front();
        signal_.cancel();  // Wake producer if it was waiting
        co_return chunk;
    }

    /// Close the queue (wakes all waiters).
    void close_queue() {
        closed = true;
        signal_.cancel();
    }
};

}  // namespace chromatindb::relay::core
