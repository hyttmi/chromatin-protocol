#include "db/crypto/verify_helpers.h"
#include "db/crypto/signing.h"

#include <catch2/catch_test_macros.hpp>
#include <asio.hpp>
#include <cstdint>
#include <vector>

using namespace chromatindb::crypto;

TEST_CASE("verify_with_offload with nullptr pool returns correct result inline",
          "[verify_helpers]") {
    // Generate a real keypair
    Signer signer;
    signer.generate_keypair();

    std::vector<uint8_t> message = {1, 2, 3, 4, 5};
    auto signature = signer.sign(message);
    auto pubkey = signer.export_public_key();

    asio::io_context io;
    bool result = false;

    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        result = co_await verify_with_offload(
            nullptr, message, signature, pubkey);
        co_return;
    }, asio::detached);

    io.run();
    REQUIRE(result == true);
}

TEST_CASE("verify_with_offload with valid pool dispatches to pool",
          "[verify_helpers]") {
    Signer signer;
    signer.generate_keypair();

    std::vector<uint8_t> message = {10, 20, 30};
    auto signature = signer.sign(message);
    auto pubkey = signer.export_public_key();

    asio::io_context io;
    asio::thread_pool pool(2);
    bool result = false;

    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        result = co_await verify_with_offload(
            &pool, message, signature, pubkey);
        co_return;
    }, asio::detached);

    io.run();
    pool.join();
    REQUIRE(result == true);
}

TEST_CASE("verify_with_offload with invalid signature returns false (no pool)",
          "[verify_helpers]") {
    Signer signer;
    signer.generate_keypair();

    std::vector<uint8_t> message = {1, 2, 3};
    std::vector<uint8_t> bad_sig(4627, 0x00);  // Wrong signature
    auto pubkey = signer.export_public_key();

    asio::io_context io;
    bool result = true;  // Start true to verify it becomes false

    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        result = co_await verify_with_offload(
            nullptr, message, bad_sig, pubkey);
        co_return;
    }, asio::detached);

    io.run();
    REQUIRE(result == false);
}

TEST_CASE("verify_with_offload with invalid signature returns false (with pool)",
          "[verify_helpers]") {
    Signer signer;
    signer.generate_keypair();

    std::vector<uint8_t> message = {4, 5, 6};
    std::vector<uint8_t> bad_sig(4627, 0xFF);  // Wrong signature
    auto pubkey = signer.export_public_key();

    asio::io_context io;
    asio::thread_pool pool(2);
    bool result = true;

    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        result = co_await verify_with_offload(
            &pool, message, bad_sig, pubkey);
        co_return;
    }, asio::detached);

    io.run();
    pool.join();
    REQUIRE(result == false);
}

TEST_CASE("verify_with_offload with valid ML-DSA-87 signature returns true",
          "[verify_helpers]") {
    Signer signer;
    signer.generate_keypair();

    // Sign a larger message to exercise the full ML-DSA-87 path
    std::vector<uint8_t> message(256);
    for (size_t i = 0; i < message.size(); ++i) {
        message[i] = static_cast<uint8_t>(i & 0xFF);
    }

    auto signature = signer.sign(message);
    auto pubkey = signer.export_public_key();

    // Test both with and without pool
    asio::io_context io;
    asio::thread_pool pool(2);
    bool result_no_pool = false;
    bool result_with_pool = false;

    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        result_no_pool = co_await verify_with_offload(
            nullptr, message, signature, pubkey);
        result_with_pool = co_await verify_with_offload(
            &pool, message, signature, pubkey);
        co_return;
    }, asio::detached);

    io.run();
    pool.join();
    REQUIRE(result_no_pool == true);
    REQUIRE(result_with_pool == true);
}
