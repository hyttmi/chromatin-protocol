#include "relay/core/session.h"

#include <spdlog/spdlog.h>

namespace chromatindb::relay::core {

static constexpr auto use_nothrow = asio::as_tuple(asio::use_awaitable);

Session::Session(asio::any_io_executor executor, size_t max_queue)
    : send_signal_(executor)
    , max_queue_(max_queue) {
}

asio::awaitable<bool> Session::enqueue(std::string message) {
    if (closed_) co_return false;

    if (send_queue_.size() >= max_queue_) {
        spdlog::warn("send queue full ({} messages), disconnecting session", max_queue_);
        close();
        co_return false;
    }

    bool result = false;
    asio::steady_timer completion(send_signal_.get_executor());
    completion.expires_after(std::chrono::hours(24));

    send_queue_.push_back({std::move(message), &completion, &result});
    send_signal_.cancel();  // Wake drain coroutine

    auto [ec] = co_await completion.async_wait(use_nothrow);
    // ec == operation_aborted means drain coroutine processed our message
    co_return result;
}

asio::awaitable<void> Session::drain_send_queue() {
    drain_running_ = true;

    while (!closed_) {
        while (!send_queue_.empty() && !closed_) {
            auto msg = std::move(send_queue_.front());
            send_queue_.pop_front();

            bool ok = co_await do_send(msg.data);

            // Signal the waiting caller
            if (msg.result_ptr) *msg.result_ptr = ok;
            if (msg.completion) msg.completion->cancel();

            if (!ok) {
                // Write failed -- drain remaining messages as failures
                while (!send_queue_.empty()) {
                    auto& m = send_queue_.front();
                    if (m.result_ptr) *m.result_ptr = false;
                    if (m.completion) m.completion->cancel();
                    send_queue_.pop_front();
                }
                break;
            }
        }

        if (closed_) break;

        // Wait for new messages (timer-cancel wakeup pattern)
        send_signal_.expires_after(std::chrono::hours(24));
        auto [ec] = co_await send_signal_.async_wait(use_nothrow);
        // ec == operation_aborted means new message was enqueued (cancel woke us)
    }

    // Drain remaining on close: signal all waiters as failed
    while (!send_queue_.empty()) {
        auto& m = send_queue_.front();
        if (m.result_ptr) *m.result_ptr = false;
        if (m.completion) m.completion->cancel();
        send_queue_.pop_front();
    }

    drain_running_ = false;
}

asio::awaitable<bool> Session::do_send(const std::string& data) {
    // Phase 100: stub write -- just record the message for testing
    delivered_.push_back(data);
    co_return true;
}

void Session::close() {
    if (closed_) return;
    closed_ = true;
    send_signal_.cancel();  // Wake drain coroutine so it can clean up

    // Also directly cancel all pending completion timers.
    // This ensures enqueue callers get unblocked even if drain is not running.
    while (!send_queue_.empty()) {
        auto& m = send_queue_.front();
        if (m.result_ptr) *m.result_ptr = false;
        if (m.completion) m.completion->cancel();
        send_queue_.pop_front();
    }
}

bool Session::is_closed() const {
    return closed_;
}

const std::deque<std::string>& Session::delivered() const {
    return delivered_;
}

} // namespace chromatindb::relay::core
