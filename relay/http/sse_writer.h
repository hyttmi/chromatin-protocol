#pragma once

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include <deque>
#include <functional>
#include <string>
#include <string_view>

namespace chromatindb::relay::http {

/// Server-Sent Events writer for long-lived HTTP connections.
///
/// Manages an event queue, drain loop, and 30-second heartbeat timer.
/// Events are formatted per SSE spec (RFC 8895 / W3C EventSource).
///
/// WriteFn is a type-erased async write function that decouples SseWriter
/// from the TLS/plain stream variant -- HttpConnection provides a WriteFn
/// that internally handles the stream branching.
///
/// Queue cap: 256 events (same as core::Session). Overflow closes the writer.
class SseWriter {
public:
    using WriteFn = std::function<asio::awaitable<bool>(std::string_view data)>;

    SseWriter(asio::any_io_executor executor, WriteFn write_fn);

    /// Push a notification event with an event ID. Thread-safe via post to executor.
    void push_event(const nlohmann::json& data, uint64_t event_id);

    /// Push a broadcast event without an event ID.
    void push_broadcast(const nlohmann::json& data);

    /// Main drain + heartbeat coroutine. Call via co_spawn.
    asio::awaitable<void> run();

    /// Stop the writer. Wakes the drain loop.
    void close();

    /// Whether the writer has been closed (by overflow, write failure, or explicit close).
    bool is_closed() const { return closed_; }

    /// Format an SSE event string: "id: N\ndata: {json}\n\n"
    static std::string format_event(const nlohmann::json& data, uint64_t event_id);

    /// Format an SSE heartbeat comment: ": heartbeat\n\n"
    static std::string format_heartbeat();

    /// Format an SSE broadcast event (no id field): "data: {json}\n\n"
    static std::string format_broadcast(const nlohmann::json& data);

private:
    static constexpr size_t MAX_QUEUE_SIZE = 256;
    static constexpr auto HEARTBEAT_INTERVAL = std::chrono::seconds(30);

    WriteFn write_fn_;
    asio::steady_timer signal_;
    std::deque<std::string> events_;
    bool closed_ = false;
};

} // namespace chromatindb::relay::http
