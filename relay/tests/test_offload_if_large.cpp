#include <catch2/catch_test_macros.hpp>
#include "relay/util/offload_if_large.h"
#include <asio.hpp>
#include <thread>
#include <vector>

using namespace chromatindb::relay::util;

TEST_CASE("offload_if_large threshold constant", "[offload]") {
    REQUIRE(OFFLOAD_THRESHOLD == 65536);
}

TEST_CASE("offload_if_large inline path for small payload", "[offload][threshold]") {
    asio::io_context ioc;
    asio::thread_pool pool(1);

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        auto result = co_await offload_if_large(pool, ioc, 100, [&] {
            return 42;
        });
        REQUIRE(result == 42);
    }, asio::detached);

    ioc.run();
}

TEST_CASE("offload_if_large inline at exact threshold", "[offload][threshold]") {
    asio::io_context ioc;
    asio::thread_pool pool(1);

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        auto result = co_await offload_if_large(pool, ioc, OFFLOAD_THRESHOLD, [&] {
            return std::string("inline");
        });
        REQUIRE(result == "inline");
    }, asio::detached);

    ioc.run();
}

TEST_CASE("offload_if_large offloads above threshold", "[offload][threshold]") {
    asio::io_context ioc;
    asio::thread_pool pool(1);
    std::thread::id ioc_tid;
    std::thread::id lambda_tid;

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        ioc_tid = std::this_thread::get_id();
        auto result = co_await offload_if_large(pool, ioc, OFFLOAD_THRESHOLD + 1, [&] {
            lambda_tid = std::this_thread::get_id();
            return 99;
        });
        // After offload_if_large returns, we are back on the ioc thread
        REQUIRE(std::this_thread::get_id() == ioc_tid);
        REQUIRE(result == 99);
    }, asio::detached);

    ioc.run();
    // Lambda ran on pool thread, not ioc thread
    REQUIRE(lambda_tid != ioc_tid);
}

TEST_CASE("offload_if_large zero size stays inline", "[offload][threshold]") {
    asio::io_context ioc;
    asio::thread_pool pool(1);

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        auto result = co_await offload_if_large(pool, ioc, 0, [&] {
            return std::vector<uint8_t>{1, 2, 3};
        });
        REQUIRE(result.size() == 3);
    }, asio::detached);

    ioc.run();
}

TEST_CASE("offload_if_large transfer-back after offload", "[offload]") {
    asio::io_context ioc;
    asio::thread_pool pool(2);
    std::thread::id ioc_tid;

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        ioc_tid = std::this_thread::get_id();
        // First offload
        co_await offload_if_large(pool, ioc, 100000, [&] { return 1; });
        // Must be back on ioc thread
        REQUIRE(std::this_thread::get_id() == ioc_tid);
        // Second offload (verifies transfer-back works repeatedly)
        co_await offload_if_large(pool, ioc, 100000, [&] { return 2; });
        REQUIRE(std::this_thread::get_id() == ioc_tid);
    }, asio::detached);

    ioc.run();
}
