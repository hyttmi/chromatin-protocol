#include "relay/http/sse_writer.h"

#include <spdlog/spdlog.h>

namespace chromatindb::relay::http {

static constexpr auto use_nothrow = asio::as_tuple(asio::use_awaitable);

SseWriter::SseWriter(asio::any_io_executor executor, WriteFn write_fn)
    : write_fn_(std::move(write_fn))
    , signal_(executor) {
}

// ---------------------------------------------------------------------------
// Static formatting methods (SSE spec)
// ---------------------------------------------------------------------------

std::string SseWriter::format_event(const nlohmann::json& data, uint64_t event_id) {
    std::string result;
    result += "id: ";
    result += std::to_string(event_id);
    result += '\n';
    result += "data: ";
    result += data.dump();
    result += '\n';
    result += '\n';
    return result;
}

std::string SseWriter::format_heartbeat() {
    return ": heartbeat\n\n";
}

std::string SseWriter::format_broadcast(const nlohmann::json& data) {
    std::string result;
    result += "data: ";
    result += data.dump();
    result += '\n';
    result += '\n';
    return result;
}

// ---------------------------------------------------------------------------
// Event queue (push methods)
// ---------------------------------------------------------------------------

void SseWriter::push_event(const nlohmann::json& data, uint64_t event_id) {
    if (closed_) return;

    if (events_.size() >= MAX_QUEUE_SIZE) {
        spdlog::warn("SSE event queue overflow ({} events), closing writer", MAX_QUEUE_SIZE);
        close();
        return;
    }

    events_.push_back(format_event(data, event_id));
    signal_.cancel();  // Wake the drain loop
}

void SseWriter::push_broadcast(const nlohmann::json& data) {
    if (closed_) return;

    if (events_.size() >= MAX_QUEUE_SIZE) {
        spdlog::warn("SSE event queue overflow ({} events), closing writer", MAX_QUEUE_SIZE);
        close();
        return;
    }

    events_.push_back(format_broadcast(data));
    signal_.cancel();  // Wake the drain loop
}

// ---------------------------------------------------------------------------
// Drain + heartbeat coroutine
// ---------------------------------------------------------------------------

asio::awaitable<void> SseWriter::run() {
    while (!closed_) {
        // Drain all queued events
        while (!events_.empty() && !closed_) {
            auto event = std::move(events_.front());
            events_.pop_front();

            bool ok = co_await write_fn_(event);
            if (!ok) {
                spdlog::debug("SSE write failed, closing writer");
                closed_ = true;
                co_return;
            }
        }

        if (closed_) break;

        // Wait for new events or heartbeat interval
        signal_.expires_after(HEARTBEAT_INTERVAL);
        auto [ec] = co_await signal_.async_wait(use_nothrow);

        // ec == operation_aborted means signal was cancelled (new event pushed)
        // ec == success means timer expired (heartbeat time)

        if (!ec && events_.empty() && !closed_) {
            // Timer expired naturally and no new events -- send heartbeat
            bool ok = co_await write_fn_(format_heartbeat());
            if (!ok) {
                spdlog::debug("SSE heartbeat write failed, closing writer");
                closed_ = true;
                co_return;
            }
        }
        // If ec == operation_aborted, loop back to drain new events
    }

    // Clear remaining events on close
    events_.clear();
}

// ---------------------------------------------------------------------------
// Close
// ---------------------------------------------------------------------------

void SseWriter::close() {
    if (closed_) return;
    closed_ = true;
    signal_.cancel();  // Wake the drain loop so it exits
}

} // namespace chromatindb::relay::http
