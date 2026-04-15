#include <catch2/catch_test_macros.hpp>
#include "relay/core/chunked_stream.h"

#include <asio.hpp>

using namespace chromatindb::relay::core;
using namespace chromatindb::relay::util;

// =============================================================================
// Header encode/decode tests
// =============================================================================

TEST_CASE("chunked_stream: encode_chunked_header produces 14-byte header",
          "[chunked_stream]") {
    auto header = encode_chunked_header(8, 42, 5242880);
    REQUIRE(header.size() == 14);
    REQUIRE(header[0] == CHUNKED_BEGIN);
    REQUIRE(header[1] == 8);
    REQUIRE(read_u32_be(header.data() + 2) == 42);
    REQUIRE(read_u64_be(header.data() + 6) == 5242880);
}

TEST_CASE("chunked_stream: encode_chunked_header with extra metadata",
          "[chunked_stream]") {
    std::vector<uint8_t> extra = {0x01};
    auto header = encode_chunked_header(9, 100, 1048576, extra);
    REQUIRE(header.size() == 15);
    REQUIRE(header[0] == CHUNKED_BEGIN);
    REQUIRE(header[1] == 9);
    REQUIRE(read_u32_be(header.data() + 2) == 100);
    REQUIRE(read_u64_be(header.data() + 6) == 1048576);
    REQUIRE(header[14] == 0x01);
}

TEST_CASE("chunked_stream: decode_chunked_header roundtrips",
          "[chunked_stream]") {
    std::vector<uint8_t> extra = {0xAA, 0xBB};
    auto encoded = encode_chunked_header(17, 99999, 524288000ULL, extra);
    auto decoded = decode_chunked_header(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->type == 17);
    REQUIRE(decoded->request_id == 99999);
    REQUIRE(decoded->total_payload_size == 524288000ULL);
    REQUIRE(decoded->extra_metadata.size() == 2);
    REQUIRE(decoded->extra_metadata[0] == 0xAA);
    REQUIRE(decoded->extra_metadata[1] == 0xBB);
}

TEST_CASE("chunked_stream: decode_chunked_header rejects short data",
          "[chunked_stream]") {
    std::vector<uint8_t> too_short(10, 0x01);
    auto decoded = decode_chunked_header(too_short);
    REQUIRE_FALSE(decoded.has_value());
}

TEST_CASE("chunked_stream: decode_chunked_header rejects wrong flags",
          "[chunked_stream]") {
    std::vector<uint8_t> bad_flags(14, 0x00);
    auto decoded = decode_chunked_header(bad_flags);
    REQUIRE_FALSE(decoded.has_value());
}

TEST_CASE("chunked_stream: decode_chunked_header without extra metadata",
          "[chunked_stream]") {
    auto encoded = encode_chunked_header(8, 1, 2048);
    auto decoded = decode_chunked_header(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->type == 8);
    REQUIRE(decoded->request_id == 1);
    REQUIRE(decoded->total_payload_size == 2048);
    REQUIRE(decoded->extra_metadata.empty());
}

// =============================================================================
// ChunkQueue tests
// =============================================================================

TEST_CASE("chunked_stream: ChunkQueue push/pop single chunk",
          "[chunked_stream]") {
    asio::io_context ioc;
    ChunkQueue queue(ioc.get_executor());

    std::vector<uint8_t> input = {1, 2, 3, 4, 5};
    std::optional<std::vector<uint8_t>> output;

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        co_await queue.push(input);
    }, asio::detached);

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        output = co_await queue.pop();
    }, asio::detached);

    ioc.run();

    REQUIRE(output.has_value());
    REQUIRE(*output == input);
}

TEST_CASE("chunked_stream: ChunkQueue backpressure blocks at MAX_DEPTH",
          "[chunked_stream]") {
    asio::io_context ioc;
    ChunkQueue queue(ioc.get_executor());

    // Push CHUNK_QUEUE_MAX_DEPTH chunks synchronously (they should fit)
    for (size_t i = 0; i < CHUNK_QUEUE_MAX_DEPTH; ++i) {
        asio::co_spawn(ioc, [&, i]() -> asio::awaitable<void> {
            co_await queue.push(std::vector<uint8_t>(1, static_cast<uint8_t>(i)));
        }, asio::detached);
    }
    ioc.run();
    ioc.restart();

    REQUIRE(queue.chunks.size() == CHUNK_QUEUE_MAX_DEPTH);

    // Push one more in a coroutine (should block until we pop one)
    bool fifth_pushed = false;
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        co_await queue.push(std::vector<uint8_t>{0xFF});
        fifth_pushed = true;
    }, asio::detached);

    // Run briefly -- the fifth push should NOT have completed yet
    ioc.poll();
    REQUIRE_FALSE(fifth_pushed);

    // Pop one chunk to make room
    std::optional<std::vector<uint8_t>> popped;
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        popped = co_await queue.pop();
    }, asio::detached);

    ioc.run();
    REQUIRE(popped.has_value());
    REQUIRE(fifth_pushed);
}

TEST_CASE("chunked_stream: ChunkQueue close wakes waiters",
          "[chunked_stream]") {
    asio::io_context ioc;
    ChunkQueue queue(ioc.get_executor());

    std::optional<std::vector<uint8_t>> result;
    bool consumer_done = false;

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        result = co_await queue.pop();
        consumer_done = true;
    }, asio::detached);

    // Run briefly -- consumer should be waiting
    ioc.poll();
    REQUIRE_FALSE(consumer_done);

    // Close the queue
    queue.close_queue();
    ioc.run();

    REQUIRE(consumer_done);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("chunked_stream: ChunkQueue push returns false when closed",
          "[chunked_stream]") {
    asio::io_context ioc;
    ChunkQueue queue(ioc.get_executor());
    queue.close_queue();

    bool push_result = true;
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        push_result = co_await queue.push(std::vector<uint8_t>{1});
    }, asio::detached);

    ioc.run();
    REQUIRE_FALSE(push_result);
}

TEST_CASE("chunked_stream: STREAMING_THRESHOLD equals CHUNK_SIZE",
          "[chunked_stream]") {
    REQUIRE(STREAMING_THRESHOLD == CHUNK_SIZE);
    static_assert(STREAMING_THRESHOLD == CHUNK_SIZE,
                  "streaming threshold must match chunk size");
}
