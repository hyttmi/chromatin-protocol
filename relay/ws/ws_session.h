#pragma once

#include "relay/core/session.h"
#include "relay/ws/ws_frame.h"

#include <asio.hpp>
#include <asio/ssl.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

namespace chromatindb::relay::ws {

class SessionManager;  // Forward declare

/// WebSocket session wrapping core::Session with TLS/plain dual-mode stream.
/// Handles RFC 6455 lifecycle: frame parsing, fragment reassembly, ping/pong
/// keepalive, close handshake, idle timeout, and write callback injection.
class WsSession : public std::enable_shared_from_this<WsSession> {
public:
    /// TLS stream type.
    using TlsStream = asio::ssl::stream<asio::ip::tcp::socket>;
    /// Dual-mode stream: plain TCP or TLS.
    using Stream = std::variant<asio::ip::tcp::socket, TlsStream>;

    /// Factory: create session after successful WS upgrade.
    static std::shared_ptr<WsSession> create(
        Stream stream,
        SessionManager& manager,
        asio::any_io_executor executor,
        size_t max_send_queue);

    /// Start the session lifecycle (read loop + drain + ping + idle timer).
    void start(uint64_t session_id);

    /// Initiate graceful close with status code + reason.
    void close(uint16_t code = CLOSE_NORMAL, std::string_view reason = "");

    uint64_t id() const { return session_id_; }

    /// Called by Session drain coroutine via WriteCallback.
    asio::awaitable<bool> write_frame(std::span<const uint8_t> data);

private:
    WsSession(Stream stream, SessionManager& manager,
              asio::any_io_executor executor, size_t max_send_queue);

    /// Main read loop: reads raw bytes, parses frames, handles control frames.
    asio::awaitable<void> read_loop();

    /// Keepalive: ping every 30s, disconnect if no pong in 60s (per D-15).
    asio::awaitable<void> ping_loop();

    /// Handle a complete assembled message (text or binary).
    void on_message(uint8_t opcode, std::vector<uint8_t> data);

    /// Handle control frames inline.
    asio::awaitable<void> handle_control(const AssembledMessage& msg);

    /// Low-level async read/write for both plain and TLS streams.
    asio::awaitable<size_t> async_read(asio::mutable_buffer buf);
    asio::awaitable<size_t> async_write(asio::const_buffer buf);

    /// Send a raw frame (bypassing send queue -- for control frames).
    asio::awaitable<bool> send_raw(const std::string& frame);

    /// Close the underlying socket.
    void shutdown_socket();

    Stream stream_;
    core::Session session_;         // Send queue (from core/)
    SessionManager& manager_;
    asio::any_io_executor executor_;
    FragmentAssembler assembler_;
    uint64_t session_id_ = 0;
    bool closing_ = false;

    // Keepalive state (per D-15)
    std::chrono::steady_clock::time_point last_pong_;
    bool first_message_received_ = false;

    // Idle timeout: 30s after WS upgrade if no message received (per D-14)
    asio::steady_timer idle_timer_;
    static constexpr auto IDLE_TIMEOUT = std::chrono::seconds(30);

    // Read buffer (persistent across reads)
    static constexpr size_t READ_BUF_SIZE = 8192;
    std::array<uint8_t, READ_BUF_SIZE> read_buf_{};
    std::vector<uint8_t> pending_data_;  // Accumulated unparsed bytes
};

} // namespace chromatindb::relay::ws
