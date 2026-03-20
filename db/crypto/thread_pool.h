#pragma once

#include <asio.hpp>
#include <asio/awaitable.hpp>
#include <asio/use_awaitable.hpp>

#include <cstdint>
#include <concepts>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

namespace chromatindb::crypto {

/// Resolve worker_threads config: 0 -> hardware_concurrency(), clamp to hw max.
inline uint32_t resolve_worker_threads(uint32_t configured) {
    uint32_t hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 2;  // Fallback if hardware_concurrency() returns 0
    if (configured == 0) return hw;
    if (configured > hw) return hw;  // Clamped (caller logs warning)
    return configured;
}

/// Dispatch a callable to the thread pool and co_await its result.
/// Uses asio::post instead of co_spawn to avoid allocating an intermediate
/// coroutine frame for a synchronous function call.
///
/// NOTE: The coroutine resumes on the thread_pool thread after co_await.
/// Callers that access io_context-bound state after offload() must transfer
/// back via co_await asio::post(executor, asio::use_awaitable) first.
template <typename F>
    requires std::invocable<F>
asio::awaitable<std::invoke_result_t<F>> offload(asio::thread_pool& pool, F&& fn) {
    using R = std::invoke_result_t<F>;
    co_return co_await async_initiate<decltype(asio::use_awaitable),
                                      void(std::error_code, R)>(
        [&pool](auto handler, auto f) {
            asio::post(pool, [h = std::move(handler), f = std::move(f)]() mutable {
                auto result = f();
                std::move(h)(std::error_code{}, std::move(result));
            });
        },
        asio::use_awaitable, std::forward<F>(fn));
}

}  // namespace chromatindb::crypto
