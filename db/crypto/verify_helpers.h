#pragma once

#include "db/crypto/signing.h"
#include "db/crypto/thread_pool.h"

#include <asio/awaitable.hpp>
#include <cstdint>
#include <span>

namespace chromatindb::crypto {

/// Verify a signature with optional thread pool offload.
/// If pool is non-null, dispatches verification to the pool.
/// If pool is null, verifies inline on the current thread.
inline asio::awaitable<bool> verify_with_offload(
    asio::thread_pool* pool,
    std::span<const uint8_t> message,
    std::span<const uint8_t> signature,
    std::span<const uint8_t> public_key) {

    if (pool) {
        co_return co_await crypto::offload(*pool,
            [&]() {
                return Signer::verify(message, signature, public_key);
            });
    } else {
        co_return Signer::verify(message, signature, public_key);
    }
}

} // namespace chromatindb::crypto
