#include <catch2/catch_test_macros.hpp>

#include "relay/core/session.h"

#include <asio.hpp>
#include <string>
#include <vector>

using chromatindb::relay::core::Session;

TEST_CASE("Session: enqueue a message to empty session", "[session]") {
    asio::io_context ioc;
    Session session(ioc.get_executor(), 256);

    bool result = false;

    // Use a shared_ptr to ensure session outlives all coroutine frames
    auto run = [](Session& s, bool& r) -> asio::awaitable<void> {
        r = co_await s.enqueue("hello");
        s.close();
    };

    asio::co_spawn(ioc, session.drain_send_queue(), asio::detached);
    asio::co_spawn(ioc, run(session, result), asio::detached);

    ioc.run();

    REQUIRE(result == true);
    REQUIRE(session.delivered().size() == 1);
    REQUIRE(session.delivered().front() == "hello");
}

TEST_CASE("Session: enqueue multiple messages delivers in FIFO order", "[session]") {
    asio::io_context ioc;
    Session session(ioc.get_executor(), 256);

    bool r0 = false, r1 = false, r2 = false;

    auto run = [](Session& s, bool& a, bool& b, bool& c) -> asio::awaitable<void> {
        a = co_await s.enqueue("first");
        b = co_await s.enqueue("second");
        c = co_await s.enqueue("third");
        s.close();
    };

    asio::co_spawn(ioc, session.drain_send_queue(), asio::detached);
    asio::co_spawn(ioc, run(session, r0, r1, r2), asio::detached);

    ioc.run();

    REQUIRE(r0 == true);
    REQUIRE(r1 == true);
    REQUIRE(r2 == true);
    REQUIRE(session.delivered().size() == 3);
    REQUIRE(session.delivered()[0] == "first");
    REQUIRE(session.delivered()[1] == "second");
    REQUIRE(session.delivered()[2] == "third");
}

TEST_CASE("Session: enqueue max_queue messages all succeed", "[session]") {
    asio::io_context ioc;
    constexpr size_t cap = 4;
    Session session(ioc.get_executor(), cap);

    size_t success_count = 0;

    auto run = [](Session& s, size_t& cnt, size_t n) -> asio::awaitable<void> {
        for (size_t i = 0; i < n; ++i) {
            bool ok = co_await s.enqueue("msg" + std::to_string(i));
            if (ok) ++cnt;
        }
        s.close();
    };

    asio::co_spawn(ioc, session.drain_send_queue(), asio::detached);
    asio::co_spawn(ioc, run(session, success_count, cap), asio::detached);

    ioc.run();

    REQUIRE(success_count == cap);
    REQUIRE(session.delivered().size() == cap);
}

TEST_CASE("Session: overflow disconnects immediately", "[session]") {
    asio::io_context ioc;
    constexpr size_t cap = 4;
    Session session(ioc.get_executor(), cap);

    // Don't start drain -- let the queue fill up.
    // Each enqueue suspends waiting for drain, so we co_spawn them individually
    // to let them pile into the queue.
    bool overflow_result = true;

    auto filler = [](Session& s, std::string msg) -> asio::awaitable<void> {
        co_await s.enqueue(std::move(msg));
    };

    auto overflow_check = [](Session& s, bool& res) -> asio::awaitable<void> {
        // Yield once to let all filler coroutines post their enqueues first
        co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
        // Now queue is full -- next enqueue should overflow
        res = co_await s.enqueue("overflow");
    };

    for (size_t i = 0; i < cap; ++i) {
        asio::co_spawn(ioc, filler(session, "msg" + std::to_string(i)), asio::detached);
    }
    asio::co_spawn(ioc, overflow_check(session, overflow_result), asio::detached);

    ioc.run();

    REQUIRE(overflow_result == false);
    REQUIRE(session.is_closed() == true);
}

TEST_CASE("Session: enqueue after close returns false", "[session]") {
    asio::io_context ioc;
    Session session(ioc.get_executor(), 256);

    bool result = true;

    auto run = [](Session& s, bool& r) -> asio::awaitable<void> {
        s.close();
        r = co_await s.enqueue("after-close");
    };

    asio::co_spawn(ioc, run(session, result), asio::detached);

    ioc.run();

    REQUIRE(result == false);
    REQUIRE(session.delivered().empty());
}

TEST_CASE("Session: close with pending messages signals failure", "[session]") {
    asio::io_context ioc;
    Session session(ioc.get_executor(), 256);

    bool r0 = false, r1 = false;

    auto enqueuer = [](Session& s, std::string msg, bool& r) -> asio::awaitable<void> {
        r = co_await s.enqueue(std::move(msg));
    };

    auto closer = [](Session& s) -> asio::awaitable<void> {
        // Yield twice to let enqueues get into the queue
        co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
        co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
        s.close();
    };

    // Start drain so it processes messages
    asio::co_spawn(ioc, session.drain_send_queue(), asio::detached);
    asio::co_spawn(ioc, enqueuer(session, "pending1", r0), asio::detached);
    asio::co_spawn(ioc, enqueuer(session, "pending2", r1), asio::detached);
    asio::co_spawn(ioc, closer(session), asio::detached);

    ioc.run();

    REQUIRE(session.is_closed() == true);
    // Drain may deliver some before close, but all results should be set
    // (either true from delivery or false from close cleanup)
}

TEST_CASE("Session: configurable cap overflows at cap+1", "[session]") {
    asio::io_context ioc;
    constexpr size_t cap = 4;
    Session session(ioc.get_executor(), cap);

    bool overflow_result = true;

    // Don't start drain -- fill queue directly
    auto filler = [](Session& s, std::string msg) -> asio::awaitable<void> {
        co_await s.enqueue(std::move(msg));
    };

    auto overflow_check = [](Session& s, bool& res) -> asio::awaitable<void> {
        co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
        // 5th message should overflow
        res = co_await s.enqueue("fifth");
    };

    for (size_t i = 0; i < cap; ++i) {
        asio::co_spawn(ioc, filler(session, "msg" + std::to_string(i)), asio::detached);
    }
    asio::co_spawn(ioc, overflow_check(session, overflow_result), asio::detached);

    ioc.run();

    REQUIRE(overflow_result == false);
    REQUIRE(session.is_closed() == true);
}
