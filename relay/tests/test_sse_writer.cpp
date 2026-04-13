#include "relay/http/sse_writer.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <string>

using namespace chromatindb::relay::http;
using json = nlohmann::json;

// =============================================================================
// Static format helpers (no async required)
// =============================================================================

TEST_CASE("SseWriter::format_event produces SSE-compliant output", "[sse]") {
    SECTION("simple notification event") {
        json data = {
            {"namespace", "abcd1234"},
            {"blob_hash", "dead0000"},
            {"seq_num", 42},
            {"blob_size", 1024},
            {"is_tombstone", false}
        };
        auto result = SseWriter::format_event(data, 1);
        // Must have "id: N\ndata: {json}\n\n" format
        REQUIRE(result.starts_with("id: 1\n"));
        REQUIRE(result.find("data: ") != std::string::npos);
        REQUIRE(result.ends_with("\n\n"));

        // Extract the data payload and verify it's valid JSON
        auto data_pos = result.find("data: ");
        auto data_line = result.substr(data_pos + 6);
        // Remove trailing \n\n
        data_line = data_line.substr(0, data_line.size() - 2);
        auto parsed = json::parse(data_line);
        REQUIRE(parsed["seq_num"] == 42);
    }

    SECTION("event with large ID") {
        json data = {{"test", true}};
        auto result = SseWriter::format_event(data, 999999999);
        REQUIRE(result.starts_with("id: 999999999\n"));
        REQUIRE(result.ends_with("\n\n"));
    }

    SECTION("event with zero ID") {
        json data = {{"x", 0}};
        auto result = SseWriter::format_event(data, 0);
        REQUIRE(result.starts_with("id: 0\n"));
    }
}

TEST_CASE("SseWriter::format_heartbeat produces SSE comment", "[sse]") {
    auto result = SseWriter::format_heartbeat();
    // SSE heartbeat is a comment line: ": heartbeat\n\n"
    REQUIRE(result == ": heartbeat\n\n");
}

TEST_CASE("SseWriter::format_broadcast produces SSE event without id field", "[sse]") {
    json data = {{"type", "storage_full"}, {"message", "Storage is full"}};
    auto result = SseWriter::format_broadcast(data);
    // Must NOT have "id:" line
    REQUIRE(result.find("id:") == std::string::npos);
    // Must have "data:" line
    REQUIRE(result.find("data: ") != std::string::npos);
    REQUIRE(result.ends_with("\n\n"));

    // Parse the data line
    auto data_pos = result.find("data: ");
    auto data_line = result.substr(data_pos + 6);
    data_line = data_line.substr(0, data_line.size() - 2);
    auto parsed = json::parse(data_line);
    REQUIRE(parsed["type"] == "storage_full");
}

// =============================================================================
// Async behavior tests (using mock write function)
// =============================================================================

TEST_CASE("SseWriter push_event queues formatted events", "[sse]") {
    asio::io_context ioc;
    std::vector<std::string> written;

    auto write_fn = [&](std::string_view data) -> asio::awaitable<bool> {
        written.emplace_back(data);
        co_return true;
    };

    SseWriter writer(ioc.get_executor(), std::move(write_fn));

    json data = {{"test", true}};
    writer.push_event(data, 1);

    // Writer has queued the event (queue_size is internal -- we verify via run())
    REQUIRE_FALSE(writer.is_closed());
}

TEST_CASE("SseWriter queue overflow closes writer", "[sse]") {
    asio::io_context ioc;
    // A write function that never gets called (we overflow before running)
    auto write_fn = [](std::string_view) -> asio::awaitable<bool> {
        co_return true;
    };

    SseWriter writer(ioc.get_executor(), std::move(write_fn));

    json data = {{"x", 1}};
    // Queue cap is 256 -- push 257 events
    for (int i = 0; i < 257; ++i) {
        writer.push_event(data, static_cast<uint64_t>(i));
    }

    // After overflow, writer should be closed
    REQUIRE(writer.is_closed());
}

TEST_CASE("SseWriter run drains queued events", "[sse]") {
    asio::io_context ioc;
    std::vector<std::string> written;

    auto write_fn = [&](std::string_view data) -> asio::awaitable<bool> {
        written.emplace_back(data);
        co_return true;
    };

    SseWriter writer(ioc.get_executor(), std::move(write_fn));

    json data1 = {{"seq", 1}};
    json data2 = {{"seq", 2}};
    writer.push_event(data1, 1);
    writer.push_event(data2, 2);

    // Start drain loop, then close after one poll
    asio::co_spawn(ioc, writer.run(), asio::detached);

    // Run enough to drain
    ioc.poll();

    // Close to stop the loop
    writer.close();
    ioc.poll();

    // Both events should have been written
    REQUIRE(written.size() == 2);
    REQUIRE(written[0].find("id: 1\n") != std::string::npos);
    REQUIRE(written[1].find("id: 2\n") != std::string::npos);
}

TEST_CASE("SseWriter write failure closes writer", "[sse]") {
    asio::io_context ioc;
    int write_count = 0;

    auto write_fn = [&](std::string_view) -> asio::awaitable<bool> {
        ++write_count;
        co_return false;  // Simulate write failure
    };

    SseWriter writer(ioc.get_executor(), std::move(write_fn));

    json data = {{"test", true}};
    writer.push_event(data, 1);

    asio::co_spawn(ioc, writer.run(), asio::detached);
    ioc.poll();

    REQUIRE(writer.is_closed());
    REQUIRE(write_count == 1);
}

TEST_CASE("SseWriter close stops run loop", "[sse]") {
    asio::io_context ioc;
    std::vector<std::string> written;

    auto write_fn = [&](std::string_view data) -> asio::awaitable<bool> {
        written.emplace_back(data);
        co_return true;
    };

    SseWriter writer(ioc.get_executor(), std::move(write_fn));

    asio::co_spawn(ioc, writer.run(), asio::detached);
    ioc.poll();  // Start the loop

    writer.close();
    ioc.poll();  // Let it process the close

    REQUIRE(writer.is_closed());

    // Pushing after close should be ignored
    json data = {{"test", true}};
    writer.push_event(data, 99);
    ioc.poll();

    // Nothing written (no events before close, and push after close is ignored)
    REQUIRE(written.empty());
}

TEST_CASE("SseWriter push_broadcast queues broadcast events", "[sse]") {
    asio::io_context ioc;
    std::vector<std::string> written;

    auto write_fn = [&](std::string_view data) -> asio::awaitable<bool> {
        written.emplace_back(data);
        co_return true;
    };

    SseWriter writer(ioc.get_executor(), std::move(write_fn));

    json data = {{"type", "storage_full"}};
    writer.push_broadcast(data);

    asio::co_spawn(ioc, writer.run(), asio::detached);
    ioc.poll();

    writer.close();
    ioc.poll();

    REQUIRE(written.size() == 1);
    // Broadcast events don't have id: line
    REQUIRE(written[0].find("id:") == std::string::npos);
    REQUIRE(written[0].find("data: ") != std::string::npos);
}
