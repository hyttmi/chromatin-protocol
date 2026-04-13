#pragma once

#include "relay/core/authenticator.h"
#include "relay/core/rate_limiter.h"
#include "relay/core/session.h"
#include "relay/ws/ws_frame.h"

namespace chromatindb::relay::core {
class UdsMultiplexer;
class RequestRouter;
class SubscriptionTracker;
struct RelayMetrics;
} // namespace chromatindb::relay::core

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

namespace chromatindb::relay::ws {

class SessionManager;  // Forward declare

/// Per D-11: explicit state enum for auth lifecycle.
enum class SessionState : uint8_t {
    AWAITING_AUTH,
    AUTHENTICATED
};

/// Per D-04: custom WebSocket close code for all auth failures.
constexpr uint16_t CLOSE_AUTH_FAILURE = 4001;

/// WebSocket session wrapping core::Session with TLS/plain dual-mode stream.
/// Handles RFC 6455 lifecycle: frame parsing, fragment reassembly, ping/pong
/// keepalive, close handshake, auth state machine, and write callback injection.
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
        size_t max_send_queue,
        core::Authenticator& authenticator,
        asio::io_context& ioc,
        core::UdsMultiplexer* uds_mux = nullptr,
        core::RequestRouter* router = nullptr,
        core::SubscriptionTracker* tracker = nullptr,
        core::RelayMetrics* metrics = nullptr,
        const std::atomic<uint32_t>* shared_rate = nullptr,
        const std::atomic<uint32_t>* max_blob_size = nullptr);

    /// Start the session lifecycle (read loop + drain + ping + auth challenge).
    void start(uint64_t session_id);

    /// Initiate graceful close with status code + reason.
    void close(uint16_t code = CLOSE_NORMAL, std::string_view reason = "");

    uint64_t id() const { return session_id_; }

    /// Called by Session drain coroutine via WriteCallback.
    asio::awaitable<bool> write_frame(std::span<const uint8_t> data);

    /// Enqueue JSON text frame via send queue (fire-and-forget).
    void send_json(const nlohmann::json& j);

private:
    WsSession(Stream stream, SessionManager& manager,
              asio::any_io_executor executor, size_t max_send_queue,
              core::Authenticator& authenticator, asio::io_context& ioc,
              core::UdsMultiplexer* uds_mux, core::RequestRouter* router,
              core::SubscriptionTracker* tracker,
              core::RelayMetrics* metrics, const std::atomic<uint32_t>* shared_rate,
              const std::atomic<uint32_t>* max_blob_size);

    /// Parse "namespaces" array of hex strings from JSON into Namespace32 vector.
    static std::vector<std::array<uint8_t, 32>> parse_namespace_list(const nlohmann::json& j);

    /// Encode namespace list with u16BE count prefix for node wire format.
    static std::vector<uint8_t> encode_namespace_list_u16be(
        const std::vector<std::array<uint8_t, 32>>& namespaces);

    /// Main read loop: reads raw bytes, parses frames, handles control frames.
    asio::awaitable<void> read_loop();

    /// Keepalive: ping every 30s, disconnect if no pong in 60s (per D-15).
    asio::awaitable<void> ping_loop();

    /// Handle a complete assembled message (text or binary).
    asio::awaitable<void> on_message(uint8_t opcode, std::vector<uint8_t> data);

    /// Handle auth message in AWAITING_AUTH state.
    asio::awaitable<void> handle_auth_message(std::vector<uint8_t> data);

    /// Send challenge JSON and start auth timer (per D-02).
    void send_challenge();

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

    // Auth state (per D-11, D-06)
    core::Authenticator& authenticator_;
    asio::io_context& ioc_;                          // For asio::post offload
    SessionState state_ = SessionState::AWAITING_AUTH;
    std::array<uint8_t, 32> challenge_{};            // Nonce sent to client
    std::vector<uint8_t> client_pubkey_;             // Stored after auth (2592 bytes, per D-06)
    std::array<uint8_t, 32> client_namespace_{};     // SHA3-256(pubkey, per D-06)
    asio::steady_timer auth_timer_;                  // 10s auth timeout (per D-02)
    static constexpr auto AUTH_TIMEOUT = std::chrono::seconds(10);  // Per D-33: hardcoded

    // Keepalive state (per D-15)
    std::chrono::steady_clock::time_point last_pong_;

    // Idle timeout: 30s after auth if no message received (per D-14)
    asio::steady_timer idle_timer_;
    static constexpr auto IDLE_TIMEOUT = std::chrono::seconds(30);

    // UDS forwarding (Phase 103)
    core::UdsMultiplexer* uds_mux_ = nullptr;
    core::RequestRouter* router_ = nullptr;

    // Subscription tracking (Phase 104)
    core::SubscriptionTracker* tracker_ = nullptr;

    // Rate limiting (Phase 105 -- per D-07, D-09, D-11, D-14)
    core::RateLimiter rate_limiter_;
    core::RelayMetrics* metrics_ = nullptr;
    const std::atomic<uint32_t>* shared_rate_ = nullptr;
    const std::atomic<uint32_t>* max_blob_size_ = nullptr;
    static constexpr uint32_t RATE_LIMIT_DISCONNECT_THRESHOLD = 10;

    // Read buffer (persistent across reads)
    static constexpr size_t READ_BUF_SIZE = 8192;
    std::array<uint8_t, READ_BUF_SIZE> read_buf_{};
    std::vector<uint8_t> pending_data_;  // Accumulated unparsed bytes
};

} // namespace chromatindb::relay::ws
