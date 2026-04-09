#include "relay/ws/ws_session.h"
#include "relay/ws/session_manager.h"

#include <spdlog/spdlog.h>

#include <chrono>

namespace chromatindb::relay::ws {

static constexpr auto use_nothrow = asio::as_tuple(asio::use_awaitable);

// ---------------------------------------------------------------------------
// Construction / Factory
// ---------------------------------------------------------------------------

WsSession::WsSession(Stream stream, SessionManager& manager,
                     asio::any_io_executor executor, size_t max_send_queue)
    : stream_(std::move(stream))
    , session_(executor, max_send_queue)
    , manager_(manager)
    , executor_(executor)
    , idle_timer_(executor) {
    last_pong_ = std::chrono::steady_clock::now();
}

std::shared_ptr<WsSession> WsSession::create(
    Stream stream,
    SessionManager& manager,
    asio::any_io_executor executor,
    size_t max_send_queue) {
    // Cannot use make_shared due to private constructor.
    return std::shared_ptr<WsSession>(
        new WsSession(std::move(stream), manager, executor, max_send_queue));
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void WsSession::start(uint64_t session_id) {
    session_id_ = session_id;

    // Inject write callback -- shared_from_this() is safe here (called after create()).
    auto self = shared_from_this();
    session_.set_write_callback(
        [self](std::span<const uint8_t> data) -> asio::awaitable<bool> {
            return self->write_frame(data);
        });

    // Start idle timer (per D-14): 30s timeout until first message.
    idle_timer_.expires_after(IDLE_TIMEOUT);
    asio::co_spawn(executor_,
        [self]() -> asio::awaitable<void> {
            auto [ec] = co_await self->idle_timer_.async_wait(use_nothrow);
            if (ec) co_return;  // Cancelled (first message arrived)
            if (!self->first_message_received_ && !self->closing_) {
                spdlog::warn("session {} idle timeout -- no message after WS upgrade", self->session_id_);
                self->close(CLOSE_PROTOCOL_ERROR, "idle timeout");
            }
        }, asio::detached);

    // Spawn session coroutines.
    asio::co_spawn(executor_,
        [self]() -> asio::awaitable<void> { co_await self->read_loop(); },
        asio::detached);

    asio::co_spawn(executor_,
        [self]() -> asio::awaitable<void> { co_await self->session_.drain_send_queue(); },
        asio::detached);

    asio::co_spawn(executor_,
        [self]() -> asio::awaitable<void> { co_await self->ping_loop(); },
        asio::detached);
}

// ---------------------------------------------------------------------------
// Async I/O helpers (dual-mode: plain TCP / TLS)
// ---------------------------------------------------------------------------

asio::awaitable<size_t> WsSession::async_read(asio::mutable_buffer buf) {
    return std::visit([&](auto& stream) -> asio::awaitable<size_t> {
        auto [ec, n] = co_await stream.async_read_some(buf, use_nothrow);
        if (ec) co_return 0;
        co_return n;
    }, stream_);
}

asio::awaitable<size_t> WsSession::async_write(asio::const_buffer buf) {
    return std::visit([&](auto& stream) -> asio::awaitable<size_t> {
        auto [ec, n] = co_await asio::async_write(stream, buf, use_nothrow);
        if (ec) co_return 0;
        co_return n;
    }, stream_);
}

asio::awaitable<bool> WsSession::send_raw(const std::string& frame) {
    auto n = co_await async_write(asio::buffer(frame));
    co_return n == frame.size();
}

// ---------------------------------------------------------------------------
// Write callback (invoked by Session drain coroutine)
// ---------------------------------------------------------------------------

asio::awaitable<bool> WsSession::write_frame(std::span<const uint8_t> data) {
    if (closing_) co_return false;

    // Encode as text frame. Phase 102+ will distinguish text vs binary.
    auto frame = encode_frame(OPCODE_TEXT, data);
    auto n = co_await async_write(asio::buffer(frame));
    co_return n == frame.size();
}

// ---------------------------------------------------------------------------
// Read loop
// ---------------------------------------------------------------------------

asio::awaitable<void> WsSession::read_loop() {
    while (!closing_) {
        auto n = co_await async_read(asio::buffer(read_buf_));
        if (n == 0) break;  // Connection closed or error

        // Append to pending data buffer.
        pending_data_.insert(pending_data_.end(),
                           read_buf_.data(), read_buf_.data() + n);

        // Parse frames from pending data.
        while (!pending_data_.empty() && !closing_) {
            auto header = parse_frame_header(
                std::span<const uint8_t>(pending_data_));
            if (!header) break;  // Insufficient bytes for header

            size_t total_frame = header->header_size + header->payload_length;
            if (pending_data_.size() < total_frame) break;  // Incomplete payload

            // Validate: client frames MUST be masked (per D-10).
            if (!header->masked) {
                spdlog::warn("session {}: client frame not masked -- protocol error",
                            session_id_);
                auto frame = encode_close_frame(CLOSE_PROTOCOL_ERROR,
                                               "client frames must be masked");
                co_await send_raw(frame);
                close(CLOSE_PROTOCOL_ERROR, "client frames must be masked");
                co_return;
            }

            // Extract and unmask payload.
            std::vector<uint8_t> payload(
                pending_data_.begin() + static_cast<ptrdiff_t>(header->header_size),
                pending_data_.begin() + static_cast<ptrdiff_t>(total_frame));
            apply_mask(payload, header->mask_key);

            // Consume from pending buffer.
            pending_data_.erase(pending_data_.begin(),
                               pending_data_.begin() + static_cast<ptrdiff_t>(total_frame));

            // Feed into fragment assembler.
            auto msg = assembler_.feed(header->opcode, header->fin,
                                       std::span<const uint8_t>(payload));

            switch (msg.result) {
            case AssemblyResult::COMPLETE:
                on_message(msg.opcode, std::move(msg.data));
                break;

            case AssemblyResult::CONTROL:
                co_await handle_control(msg);
                break;

            case AssemblyResult::ERROR:
                spdlog::warn("session {}: frame assembly error: {}",
                            session_id_, msg.error_reason);
                {
                    auto frame = encode_close_frame(CLOSE_PROTOCOL_ERROR,
                                                   msg.error_reason);
                    co_await send_raw(frame);
                }
                close(CLOSE_PROTOCOL_ERROR, msg.error_reason);
                co_return;

            case AssemblyResult::INCOMPLETE:
                // More frames needed, continue reading.
                break;
            }
        }
    }

    // Read loop exited -- clean up session.
    if (!closing_) {
        spdlog::debug("session {}: connection closed by peer", session_id_);
    }
    manager_.remove_session(session_id_);
    session_.close();
    shutdown_socket();
}

// ---------------------------------------------------------------------------
// Control frame handling
// ---------------------------------------------------------------------------

asio::awaitable<void> WsSession::handle_control(const AssembledMessage& msg) {
    switch (msg.opcode) {
    case OPCODE_PING: {
        // Respond with Pong carrying same payload.
        auto pong = encode_frame(OPCODE_PONG,
                                std::span<const uint8_t>(msg.data));
        co_await send_raw(pong);
        break;
    }

    case OPCODE_PONG:
        // Update keepalive timestamp.
        last_pong_ = std::chrono::steady_clock::now();
        break;

    case OPCODE_CLOSE: {
        // Echo close frame with same status code (per D-16).
        auto close_frame = encode_close_frame(msg.close_code);
        co_await send_raw(close_frame);
        close(msg.close_code, "");
        break;
    }

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Ping/pong keepalive (per D-15)
// ---------------------------------------------------------------------------

asio::awaitable<void> WsSession::ping_loop() {
    while (!closing_) {
        asio::steady_timer timer(executor_);
        timer.expires_after(std::chrono::seconds(30));
        auto [ec] = co_await timer.async_wait(use_nothrow);
        if (ec || closing_) break;

        // Check if pong was received within 60s.
        auto elapsed = std::chrono::steady_clock::now() - last_pong_;
        if (elapsed > std::chrono::seconds(60)) {
            spdlog::warn("session {}: pong timeout ({}s since last pong)",
                        session_id_,
                        std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
            close(CLOSE_PROTOCOL_ERROR, "pong timeout");
            break;
        }

        // Send Ping frame.
        auto ping = encode_frame(OPCODE_PING, {});
        co_await send_raw(ping);
    }
}

// ---------------------------------------------------------------------------
// Message handling
// ---------------------------------------------------------------------------

void WsSession::on_message(uint8_t opcode, std::vector<uint8_t> data) {
    // Disarm idle timer on first message (per D-14).
    if (!first_message_received_) {
        first_message_received_ = true;
        idle_timer_.cancel();
    }

    // Phase 101: log only. Phase 102 adds JSON parsing and message dispatch.
    spdlog::debug("session {}: received {} frame, {} bytes",
                 session_id_,
                 opcode == OPCODE_TEXT ? "text" : "binary",
                 data.size());
}

// ---------------------------------------------------------------------------
// Close handling (per D-16)
// ---------------------------------------------------------------------------

void WsSession::close(uint16_t code, std::string_view reason) {
    if (closing_) return;
    closing_ = true;

    spdlog::debug("session {}: closing with code {} reason '{}'",
                 session_id_, code, reason);

    // Send close frame directly (bypass send queue for control frames).
    auto self = shared_from_this();
    asio::co_spawn(executor_,
        [self, code, reason = std::string(reason)]() -> asio::awaitable<void> {
            auto frame = encode_close_frame(code, reason);
            co_await self->send_raw(frame);

            // Wait up to 5 seconds for client close echo, then force shutdown.
            asio::steady_timer timer(self->executor_);
            timer.expires_after(std::chrono::seconds(5));
            auto [ec] = co_await timer.async_wait(use_nothrow);

            self->shutdown_socket();
            self->session_.close();
            self->manager_.remove_session(self->session_id_);
        }, asio::detached);
}

// ---------------------------------------------------------------------------
// Socket shutdown
// ---------------------------------------------------------------------------

void WsSession::shutdown_socket() {
    std::visit([this](auto& stream) {
        using T = std::decay_t<decltype(stream)>;
        asio::error_code ec;

        if constexpr (std::is_same_v<T, asio::ip::tcp::socket>) {
            stream.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            stream.close(ec);
        } else {
            // TLS: shutdown the SSL layer, then close the underlying socket.
            // Use sync shutdown (best-effort, we're tearing down anyway).
            stream.shutdown(ec);
            stream.lowest_layer().close(ec);
        }
    }, stream_);
}

} // namespace chromatindb::relay::ws
