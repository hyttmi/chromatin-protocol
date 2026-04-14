#pragma once

#include "relay/util/thread_pool.h"
#include <asio.hpp>
#include <cstddef>

namespace chromatindb::relay::util {

/// Offload threshold for CPU-heavy work (64 KB per D-06).
constexpr size_t OFFLOAD_THRESHOLD = 65536;

/// Conditionally offload a callable to the thread pool if payload exceeds threshold.
/// If offloaded, transfers back to the io_context event loop before returning.
/// If inline, executes directly on the event loop thread.
template <typename F>
    requires std::invocable<F>
asio::awaitable<std::invoke_result_t<F>> offload_if_large(
    asio::thread_pool& pool,
    asio::io_context& ioc,
    size_t payload_size,
    F&& fn) {
    if (payload_size > OFFLOAD_THRESHOLD) {
        auto result = co_await offload(pool, std::forward<F>(fn));
        co_await asio::post(ioc, asio::use_awaitable);
        co_return result;
    }
    co_return fn();
}

}  // namespace chromatindb::relay::util
