#pragma once

#include "relay/core/chunked_stream.h"

#include <asio.hpp>
#include <cstdint>
#include <memory>
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

/// Streaming response for large blob reads.
/// read_loop() pushes UDS chunks directly to the ChunkQueue.
/// The handler coroutine consumes from the queue and writes HTTP chunked-TE.
/// This avoids reassembling the full blob in relay memory.
struct StreamingResponsePromise {
    explicit StreamingResponsePromise(asio::any_io_executor executor)
        : header_timer_(executor), queue(executor) {}

    /// Header info delivered before data chunks.
    struct HeaderInfo {
        uint8_t type;
        uint32_t request_id;
        uint64_t total_size;
        std::vector<uint8_t> extra_metadata;  // e.g., status byte for ReadResponse
    };

    /// Called by read_loop when chunked header is received.
    void set_header(HeaderInfo info) {
        header_ = std::move(info);
        header_ready_ = true;
        header_timer_.cancel();
    }

    /// Wait for header info. Returns nullopt on timeout.
    template<typename Duration>
    asio::awaitable<std::optional<HeaderInfo>> wait_header(Duration timeout) {
        if (header_ready_) co_return std::move(header_);
        header_timer_.expires_after(timeout);
        auto [ec] = co_await header_timer_.async_wait(asio::as_tuple(asio::use_awaitable));
        if (header_ready_) co_return std::move(header_);
        co_return std::nullopt;
    }

    /// Chunk queue for incremental delivery (backpressure-aware).
    core::ChunkQueue queue;

private:
    asio::steady_timer header_timer_;
    std::optional<HeaderInfo> header_;
    bool header_ready_ = false;
};

/// Registry mapping relay_rid -> ResponsePromise for pending HTTP requests.
///
/// Access from single event loop thread -- no synchronization needed.
///
/// Promises are shared_ptr-owned by both the map and the handler coroutine.
/// This prevents use-after-free when the handler coroutine ends (timeout/disconnect)
/// while the UDS thread is resolving the same promise.
class ResponsePromiseMap {
public:
    /// Create and register a promise for a relay request ID.
    /// Returns shared_ptr that the handler coroutine must hold.
    std::shared_ptr<ResponsePromise> create_promise(uint32_t relay_rid, asio::any_io_executor executor) {
        auto promise = std::make_shared<ResponsePromise>(executor);
        promises_[relay_rid] = promise;
        return promise;
    }

    /// Resolve a promise by relay_rid. Returns true if found and resolved.
    /// Removes the entry from the map after resolution.
    /// Falls back to streaming promises if no regular promise is found --
    /// this handles the case where a small blob response arrives for a
    /// handler that registered a streaming promise (non-chunked ReadResponse).
    bool resolve(uint32_t relay_rid, uint8_t type, std::vector<uint8_t> payload) {
        auto it = promises_.find(relay_rid);
        if (it != promises_.end()) {
            auto p = it->second;
            promises_.erase(it);
            // shared_ptr keeps promise alive after map removal
            p->resolve(type, std::move(payload));
            return true;
        }
        // Fallback: check streaming promises (non-chunked response to streaming handler)
        auto sit = streaming_promises_.find(relay_rid);
        if (sit != streaming_promises_.end()) {
            auto sp = sit->second;
            streaming_promises_.erase(sit);
            // Deliver as: header with total_size = payload.size, then one chunk, then close.
            // set_header and close are synchronous. Push directly into the deque
            // (queue is empty, won't exceed MAX_DEPTH).
            sp->set_header({type, relay_rid, static_cast<uint64_t>(payload.size()), {}});
            sp->queue.chunks.push_back(std::move(payload));
            sp->queue.close_queue();
            return true;
        }
        return false;
    }

    /// Remove a promise without resolving (on timeout or client disconnect).
    void remove(uint32_t relay_rid) {
        promises_.erase(relay_rid);
    }

    /// Create a streaming promise for chunked responses (large blob reads).
    std::shared_ptr<StreamingResponsePromise> create_streaming_promise(
        uint32_t relay_rid, asio::any_io_executor executor) {
        auto promise = std::make_shared<StreamingResponsePromise>(executor);
        streaming_promises_[relay_rid] = promise;
        return promise;
    }

    /// Get streaming promise by relay_rid (for read_loop to push chunks).
    std::shared_ptr<StreamingResponsePromise> get_streaming(uint32_t relay_rid) {
        auto it = streaming_promises_.find(relay_rid);
        if (it == streaming_promises_.end()) return nullptr;
        return it->second;
    }

    /// Remove streaming promise.
    void remove_streaming(uint32_t relay_rid) {
        streaming_promises_.erase(relay_rid);
    }

    /// Cancel all promises and clear the map (for shutdown cleanup).
    void cancel_all() {
        for (auto& [rid, promise] : promises_) {
            promise->cancel();
        }
        promises_.clear();
        for (auto& [rid, sp] : streaming_promises_) {
            sp->queue.close_queue();
        }
        streaming_promises_.clear();
    }

    /// Number of pending promises (for metrics/debugging).
    size_t size() const {
        return promises_.size();
    }

    /// Number of pending streaming promises (for metrics/debugging).
    size_t streaming_size() const {
        return streaming_promises_.size();
    }

private:
    std::unordered_map<uint32_t, std::shared_ptr<ResponsePromise>> promises_;
    std::unordered_map<uint32_t, std::shared_ptr<StreamingResponsePromise>> streaming_promises_;
};

} // namespace chromatindb::relay::http
