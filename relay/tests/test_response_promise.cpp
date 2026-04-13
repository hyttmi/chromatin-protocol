#include <catch2/catch_test_macros.hpp>

#include "relay/http/response_promise.h"

#include <asio.hpp>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

using chromatindb::relay::http::ResponseData;
using chromatindb::relay::http::ResponsePromise;
using chromatindb::relay::http::ResponsePromiseMap;

// =============================================================================
// ResponsePromise tests
// =============================================================================

TEST_CASE("ResponsePromise resolve then wait returns data", "[response_promise]") {
    asio::io_context ioc;

    ResponsePromise promise(ioc.get_executor());

    // Resolve before wait
    std::vector<uint8_t> payload = {0x01, 0x02, 0x03};
    promise.resolve(42, payload);

    REQUIRE(promise.is_resolved());

    // Now co_await wait() should return immediately with the data
    std::optional<ResponseData> result;
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        result = co_await promise.wait(std::chrono::seconds(5));
    }, asio::detached);

    ioc.run();

    REQUIRE(result.has_value());
    REQUIRE(result->type == 42);
    REQUIRE(result->payload == std::vector<uint8_t>{0x01, 0x02, 0x03});
}

TEST_CASE("ResponsePromise wait then resolve wakes coroutine", "[response_promise]") {
    asio::io_context ioc;

    ResponsePromise promise(ioc.get_executor());

    std::optional<ResponseData> result;
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        result = co_await promise.wait(std::chrono::seconds(5));
    }, asio::detached);

    // Run to start the coroutine (it will suspend on timer)
    ioc.poll();
    REQUIRE(!result.has_value());
    REQUIRE(!promise.is_resolved());

    // Resolve the promise
    std::vector<uint8_t> payload = {0xAA, 0xBB};
    promise.resolve(11, payload);

    // Run to completion -- coroutine should wake
    ioc.run();

    REQUIRE(result.has_value());
    REQUIRE(result->type == 11);
    REQUIRE(result->payload == std::vector<uint8_t>{0xAA, 0xBB});
}

TEST_CASE("ResponsePromise timeout returns nullopt", "[response_promise]") {
    asio::io_context ioc;

    ResponsePromise promise(ioc.get_executor());

    std::optional<ResponseData> result;
    bool completed = false;
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        // Use very short timeout so test completes quickly
        result = co_await promise.wait(std::chrono::milliseconds(10));
        completed = true;
    }, asio::detached);

    ioc.run();

    REQUIRE(completed);
    REQUIRE(!result.has_value());  // Timeout => nullopt
    REQUIRE(!promise.is_resolved());
}

TEST_CASE("ResponsePromise cancel returns nullopt", "[response_promise]") {
    asio::io_context ioc;

    ResponsePromise promise(ioc.get_executor());

    std::optional<ResponseData> result;
    bool completed = false;
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        result = co_await promise.wait(std::chrono::seconds(60));
        completed = true;
    }, asio::detached);

    // Start the coroutine
    ioc.poll();
    REQUIRE(!completed);

    // Cancel the promise
    promise.cancel();

    // Run to completion
    ioc.run();

    REQUIRE(completed);
    REQUIRE(!result.has_value());  // Cancel without data => nullopt
    REQUIRE(!promise.is_resolved());
}

TEST_CASE("ResponsePromise is_resolved false before resolve", "[response_promise]") {
    asio::io_context ioc;

    ResponsePromise promise(ioc.get_executor());

    REQUIRE(!promise.is_resolved());
}

TEST_CASE("ResponsePromise is_resolved true after resolve", "[response_promise]") {
    asio::io_context ioc;

    ResponsePromise promise(ioc.get_executor());

    promise.resolve(1, {0xFF});
    REQUIRE(promise.is_resolved());
}

TEST_CASE("Multiple promises wake independently", "[response_promise]") {
    asio::io_context ioc;

    ResponsePromise promise1(ioc.get_executor());
    ResponsePromise promise2(ioc.get_executor());

    std::optional<ResponseData> result1;
    std::optional<ResponseData> result2;

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        result1 = co_await promise1.wait(std::chrono::seconds(5));
    }, asio::detached);

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        result2 = co_await promise2.wait(std::chrono::seconds(5));
    }, asio::detached);

    // Start coroutines
    ioc.poll();
    REQUIRE(!result1.has_value());
    REQUIRE(!result2.has_value());

    // Resolve only promise2
    promise2.resolve(22, {0x02});
    ioc.poll();

    REQUIRE(!result1.has_value());  // Still waiting
    REQUIRE(result2.has_value());
    REQUIRE(result2->type == 22);

    // Resolve promise1
    promise1.resolve(11, {0x01});
    ioc.run();

    REQUIRE(result1.has_value());
    REQUIRE(result1->type == 11);
}

// =============================================================================
// ResponsePromiseMap tests
// =============================================================================

TEST_CASE("ResponsePromiseMap register and resolve", "[response_promise_map]") {
    asio::io_context ioc;

    ResponsePromise promise(ioc.get_executor());
    ResponsePromiseMap map;

    map.register_promise(100, &promise);
    REQUIRE(map.size() == 1);

    bool resolved = map.resolve(100, 8, {0xDE, 0xAD});
    REQUIRE(resolved);
    REQUIRE(map.size() == 0);  // Removed after resolve
    REQUIRE(promise.is_resolved());
}

TEST_CASE("ResponsePromiseMap resolve unknown rid returns false", "[response_promise_map]") {
    ResponsePromiseMap map;

    bool resolved = map.resolve(999, 8, {0x01});
    REQUIRE(!resolved);
}

TEST_CASE("ResponsePromiseMap remove", "[response_promise_map]") {
    asio::io_context ioc;

    ResponsePromise promise(ioc.get_executor());
    ResponsePromiseMap map;

    map.register_promise(42, &promise);
    REQUIRE(map.size() == 1);

    map.remove(42);
    REQUIRE(map.size() == 0);

    // Resolve after remove does nothing
    bool resolved = map.resolve(42, 8, {});
    REQUIRE(!resolved);
}

TEST_CASE("ResponsePromiseMap cancel_all", "[response_promise_map]") {
    asio::io_context ioc;

    ResponsePromise p1(ioc.get_executor());
    ResponsePromise p2(ioc.get_executor());
    ResponsePromise p3(ioc.get_executor());

    ResponsePromiseMap map;
    map.register_promise(1, &p1);
    map.register_promise(2, &p2);
    map.register_promise(3, &p3);

    REQUIRE(map.size() == 3);

    // Start waiting on all promises
    std::optional<ResponseData> r1, r2, r3;
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        r1 = co_await p1.wait(std::chrono::seconds(60));
    }, asio::detached);
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        r2 = co_await p2.wait(std::chrono::seconds(60));
    }, asio::detached);
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        r3 = co_await p3.wait(std::chrono::seconds(60));
    }, asio::detached);

    ioc.poll();

    // Cancel all
    map.cancel_all();
    REQUIRE(map.size() == 0);

    ioc.run();

    // All should return nullopt (cancelled, not resolved)
    REQUIRE(!r1.has_value());
    REQUIRE(!r2.has_value());
    REQUIRE(!r3.has_value());
}

TEST_CASE("ResponsePromiseMap size tracks state", "[response_promise_map]") {
    asio::io_context ioc;

    ResponsePromise p1(ioc.get_executor());
    ResponsePromise p2(ioc.get_executor());

    ResponsePromiseMap map;
    REQUIRE(map.size() == 0);

    map.register_promise(1, &p1);
    REQUIRE(map.size() == 1);

    map.register_promise(2, &p2);
    REQUIRE(map.size() == 2);

    map.resolve(1, 8, {});
    REQUIRE(map.size() == 1);

    map.remove(2);
    REQUIRE(map.size() == 0);
}

TEST_CASE("ResponsePromiseMap resolve passes data through to promise", "[response_promise_map]") {
    asio::io_context ioc;

    ResponsePromise promise(ioc.get_executor());
    ResponsePromiseMap map;
    map.register_promise(77, &promise);

    std::vector<uint8_t> big_payload(1024, 0xCC);
    map.resolve(77, 12, big_payload);

    // Verify the promise got the data by co_awaiting
    std::optional<ResponseData> result;
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        result = co_await promise.wait(std::chrono::seconds(1));
    }, asio::detached);

    ioc.run();

    REQUIRE(result.has_value());
    REQUIRE(result->type == 12);
    REQUIRE(result->payload.size() == 1024);
    REQUIRE(result->payload[0] == 0xCC);
}
