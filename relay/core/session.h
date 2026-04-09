#pragma once

#include <asio.hpp>
#include <deque>
#include <string>
#include <cstddef>

namespace chromatindb::relay::core {

/// Per-client session with bounded send queue and drain coroutine.
/// Every outbound message flows through this queue, serializing writes
/// and bounding memory per client.
class Session {
public:
    explicit Session(asio::any_io_executor executor, size_t max_queue = 256);

    /// Enqueue a message for sending. Returns false if queue full (overflow).
    /// On overflow, session is closed immediately -- no silent message dropping.
    asio::awaitable<bool> enqueue(std::string message);

    /// Drain coroutine -- co_spawn this alongside session lifecycle.
    /// Pops messages from queue and sends them via do_send().
    asio::awaitable<void> drain_send_queue();

    /// Close the session. Signals all pending messages as failed.
    void close();

    bool is_closed() const;

    /// Access delivered messages (for testing in Phase 100; replaced in Phase 101).
    const std::deque<std::string>& delivered() const;

private:
    struct PendingMessage {
        std::string data;
        asio::steady_timer* completion;  // Owned by enqueue caller's stack
        bool* result_ptr;                // Points to local in enqueue
    };

    /// The actual write operation. In Phase 100, appends to delivered_ deque.
    /// In Phase 101, this becomes the WebSocket write.
    asio::awaitable<bool> do_send(const std::string& data);

    std::deque<PendingMessage> send_queue_;
    std::deque<std::string> delivered_;  // For Phase 100 testing
    asio::steady_timer send_signal_;
    size_t max_queue_;
    bool closed_ = false;
    bool drain_running_ = false;
};

} // namespace chromatindb::relay::core
