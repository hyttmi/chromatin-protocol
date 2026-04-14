#pragma once

#include <asio.hpp>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace chromatindb::relay::http {

/// Data returned when a UDS response arrives for a pending HTTP request.
struct ResponseData {
    uint8_t type;
    std::vector<uint8_t> payload;
};

/// One-shot awaitable for HTTP handler coroutines waiting for a UDS response.
///
/// Pattern: create on coroutine stack -> register with ResponsePromiseMap keyed by
/// relay_rid -> co_await wait(timeout) -> UDS route calls resolve() which wakes
/// the coroutine -> handler sends HTTP response.
///
/// Uses asio::steady_timer as a signal mechanism (same pattern as
/// core::Session's drain_send_queue). Timer initially expires at max timepoint.
/// resolve() cancels the timer (waking the waiter with data). Timeout triggers
/// naturally when the timer expires.
class ResponsePromise {
public:
    explicit ResponsePromise(asio::any_io_executor executor)
        : timer_(executor) {
        // Set to max timepoint -- will be overridden by wait() with actual timeout.
        timer_.expires_at(asio::steady_timer::time_point::max());
    }

    /// Wait for resolution or timeout. Returns the response data on success,
    /// or nullopt on timeout/cancel.
    ///
    /// If resolve() was called before wait(), returns immediately with the data
    /// (the timer cancel in resolve() makes async_wait return operation_aborted,
    /// and we check resolved_ to distinguish from cancel()).
    template<typename Duration>
    asio::awaitable<std::optional<ResponseData>> wait(Duration timeout) {
        if (resolved_) {
            co_return std::move(data_);
        }
        timer_.expires_after(timeout);
        auto [ec] = co_await timer_.async_wait(asio::as_tuple(asio::use_awaitable));
        // ec == operation_aborted means timer was cancelled by resolve() or cancel()
        // ec == success means timer expired (timeout)
        if (resolved_) {
            co_return std::move(data_);
        }
        co_return std::nullopt;  // Timeout or cancel without data
    }

    /// Resolve with response data. Wakes the waiting coroutine.
    /// Safe to call before wait() -- the data is stored and wait() returns immediately.
    void resolve(uint8_t type, std::vector<uint8_t> payload) {
        data_ = ResponseData{type, std::move(payload)};
        resolved_ = true;
        timer_.cancel();
    }

    /// Cancel without data (for disconnect cleanup).
    /// Wakes the waiting coroutine which returns nullopt.
    void cancel() {
        timer_.cancel();
    }

    /// Check if this promise has been resolved with data.
    bool is_resolved() const { return resolved_; }

private:
    asio::steady_timer timer_;
    std::optional<ResponseData> data_;
    bool resolved_ = false;
};

/// Registry mapping relay_rid -> ResponsePromise for pending HTTP requests.
///
/// Thread-safe via mutex -- accessed from multiple io_context threads
/// (HTTP handler coroutines register/remove, UDS read_loop resolves).
/// The map stores non-owning pointers. Each ResponsePromise is stack-allocated
/// by the HTTP handler coroutine, whose lifetime spans the full request.
class ResponsePromiseMap {
public:
    /// Register a promise for a relay request ID.
    void register_promise(uint32_t relay_rid, ResponsePromise* promise) {
        std::lock_guard lock(mu_);
        promises_[relay_rid] = promise;
    }

    /// Resolve a promise by relay_rid. Returns true if found and resolved.
    /// Removes the entry from the map after resolution.
    bool resolve(uint32_t relay_rid, uint8_t type, std::vector<uint8_t> payload) {
        ResponsePromise* p = nullptr;
        {
            std::lock_guard lock(mu_);
            auto it = promises_.find(relay_rid);
            if (it == promises_.end()) return false;
            p = it->second;
            promises_.erase(it);
        }
        // Resolve outside lock -- timer cancel is thread-safe in Asio
        p->resolve(type, std::move(payload));
        return true;
    }

    /// Remove a promise without resolving (on timeout or client disconnect).
    void remove(uint32_t relay_rid) {
        std::lock_guard lock(mu_);
        promises_.erase(relay_rid);
    }

    /// Cancel all promises and clear the map (for shutdown cleanup).
    void cancel_all() {
        std::lock_guard lock(mu_);
        for (auto& [rid, promise] : promises_) {
            promise->cancel();
        }
        promises_.clear();
    }

    /// Number of pending promises (for metrics/debugging).
    size_t size() const {
        std::lock_guard lock(mu_);
        return promises_.size();
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<uint32_t, ResponsePromise*> promises_;
};

} // namespace chromatindb::relay::http
