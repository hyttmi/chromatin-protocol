#pragma once

#include "relay/core/authenticator.h"
#include "relay/ws/session_manager.h"

namespace chromatindb::relay::core {
class UdsMultiplexer;
class RequestRouter;
class SubscriptionTracker;
struct RelayMetrics;
} // namespace chromatindb::relay::core

#include <asio.hpp>
#include <asio/ssl.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace chromatindb::relay::ws {

/// Accepts WebSocket connections over TLS or plain TCP.
/// Manages TLS context with SIGHUP reload support (per D-19).
/// Enforces configurable connection cap (per D-32) and handshake timeout (per D-13).
class WsAcceptor {
public:
    WsAcceptor(asio::io_context& ioc, SessionManager& manager,
               const std::string& bind_address, uint16_t bind_port,
               size_t max_send_queue, size_t max_connections,
               core::Authenticator& authenticator,
               core::UdsMultiplexer* uds_mux = nullptr,
               core::RequestRouter* router = nullptr,
               core::SubscriptionTracker* tracker = nullptr);

    /// Initialize TLS context. Call before accept_loop if TLS enabled.
    /// Returns false on failure (relay should exit).
    bool init_tls(const std::string& cert_path, const std::string& key_path);

    /// Reload TLS cert/key. Called from SIGHUP handler (per D-19).
    /// Returns false on failure (keeps old context).
    bool reload_tls(const std::string& cert_path, const std::string& key_path);

    /// Main accept loop coroutine. co_spawn this.
    asio::awaitable<void> accept_loop();

    /// Signal to stop accepting.
    void stop();

    bool is_tls_enabled() const;

    /// Update max connections on SIGHUP (per D-32).
    void set_max_connections(size_t n);

    /// Set shared metrics struct pointer for new sessions.
    void set_metrics(core::RelayMetrics* m) { metrics_ = m; }

    /// Set shared rate atomic pointer for new sessions (per D-14).
    void set_shared_rate(const std::atomic<uint32_t>* r) { shared_rate_ = r; }

    /// Set shared max blob size atomic pointer for new sessions (FEAT-02).
    void set_max_blob_size(const std::atomic<uint32_t>* p) { max_blob_size_ = p; }

private:
    /// Handle a new TCP connection: TLS handshake (if enabled) + WS upgrade.
    asio::awaitable<void> handle_new_connection(asio::ip::tcp::socket socket);

    /// Perform WS upgrade handshake on an already-connected stream.
    template<typename Stream>
    asio::awaitable<bool> do_ws_upgrade(Stream& stream);

    asio::ip::tcp::acceptor acceptor_;
    SessionManager& manager_;
    asio::io_context& ioc_;

    // TLS context: shared_ptr for ref-counted lifetime (per D-19).
    // Mutex protects swap (SIGHUP from thread pool per D-21).
    mutable std::mutex tls_mutex_;
    std::shared_ptr<asio::ssl::context> tls_ctx_;

    size_t max_send_queue_;
    size_t max_connections_;             // Per D-32: SIGHUP-reloadable
    core::Authenticator& authenticator_;
    core::UdsMultiplexer* uds_mux_ = nullptr;
    core::RequestRouter* router_ = nullptr;
    core::SubscriptionTracker* tracker_ = nullptr;
    core::RelayMetrics* metrics_ = nullptr;
    const std::atomic<uint32_t>* shared_rate_ = nullptr;
    const std::atomic<uint32_t>* max_blob_size_ = nullptr;
    bool stopping_ = false;

    static constexpr auto HANDSHAKE_TIMEOUT = std::chrono::seconds(5);  // Per D-13
};

} // namespace chromatindb::relay::ws
