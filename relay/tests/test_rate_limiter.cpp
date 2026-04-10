#include <catch2/catch_test_macros.hpp>

#include "relay/core/rate_limiter.h"

#include <thread>

using chromatindb::relay::core::RateLimiter;

TEST_CASE("RateLimiter: disabled mode (rate=0) allows all", "[rate_limiter]") {
    RateLimiter rl;
    // Default rate is 0 -- disabled
    for (int i = 0; i < 1000; ++i) {
        REQUIRE(rl.try_consume());
    }
}

TEST_CASE("RateLimiter: allows full burst", "[rate_limiter]") {
    RateLimiter rl;
    rl.set_rate(10);
    for (int i = 0; i < 10; ++i) {
        REQUIRE(rl.try_consume());
    }
}

TEST_CASE("RateLimiter: rejects over burst", "[rate_limiter]") {
    RateLimiter rl;
    rl.set_rate(10);
    // Consume full burst
    for (int i = 0; i < 10; ++i) {
        rl.try_consume();
    }
    // 11th should be rejected
    CHECK_FALSE(rl.try_consume());
}

TEST_CASE("RateLimiter: consecutive reject tracking", "[rate_limiter]") {
    RateLimiter rl;
    rl.set_rate(1);
    // Consume the single token
    REQUIRE(rl.try_consume());
    CHECK(rl.consecutive_rejects() == 0);

    // 10 more attempts should all fail
    for (int i = 0; i < 10; ++i) {
        CHECK_FALSE(rl.try_consume());
    }
    CHECK(rl.consecutive_rejects() == 10);
    CHECK(rl.should_disconnect(10));
    CHECK_FALSE(rl.should_disconnect(11));
}

TEST_CASE("RateLimiter: accept resets consecutive rejects", "[rate_limiter]") {
    RateLimiter rl;
    rl.set_rate(100);
    // Consume all tokens
    for (int i = 0; i < 100; ++i) {
        rl.try_consume();
    }
    // Some rejections
    CHECK_FALSE(rl.try_consume());
    CHECK_FALSE(rl.try_consume());
    CHECK(rl.consecutive_rejects() == 2);

    // Wait for tokens to refill (100 tokens/sec, ~20ms gives ~2 tokens)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(rl.try_consume());
    CHECK(rl.consecutive_rejects() == 0);
}

TEST_CASE("RateLimiter: set_rate resets bucket", "[rate_limiter]") {
    RateLimiter rl;
    rl.set_rate(5);
    // Exhaust bucket
    for (int i = 0; i < 5; ++i) {
        rl.try_consume();
    }
    CHECK_FALSE(rl.try_consume());

    // Reset with same rate -- should refill bucket
    rl.set_rate(5);
    CHECK(rl.try_consume());
}
