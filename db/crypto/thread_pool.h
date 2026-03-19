#pragma once

#include <asio.hpp>
#include <asio/awaitable.hpp>
#include <asio/use_awaitable.hpp>

#include <cstdint>
#include <concepts>
#include <thread>
#include <type_traits>

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
/// The callable runs on a pool worker; the coroutine resumes on the caller's executor.
template <typename Executor, typename F>
    requires std::invocable<F>
asio::awaitable<std::invoke_result_t<F>> offload(asio::thread_pool& pool, Executor caller_ex, F&& fn) {
    using R = std::invoke_result_t<F>;
    co_return co_await asio::co_spawn(pool, [f = std::forward<F>(fn)]() -> asio::awaitable<R> {
        co_return f();
    }, asio::use_awaitable);
}

}  // namespace chromatindb::crypto
