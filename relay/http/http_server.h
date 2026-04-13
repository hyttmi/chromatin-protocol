#pragma once

#include <asio.hpp>
#include <asio/ssl.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace chromatindb::relay::core {
class SubscriptionTracker;  // Forward declaration
class UdsMultiplexer;       // Forward declaration
} // namespace chromatindb::relay::core

namespace chromatindb::relay::http {

class HttpRouter;
class TokenStore;

/// HTTP/1.1 server with optional TLS, coroutine-per-connection model.
/// Accept loop pattern follows MetricsCollector; TLS dual-mode with SIGHUP reload.
/// Per D-03, D-35, D-36, D-37, D-38.
class HttpServer {
public:
    HttpServer(asio::io_context& ioc, HttpRouter& router, TokenStore& token_store,
               core::SubscriptionTracker& tracker, core::UdsMultiplexer& uds,
               const std::string& bind_address, uint16_t bind_port,
               uint32_t max_connections, const std::atomic<bool>& stopping);

    /// Initialize TLS context (TLS 1.3 server). Call before accept_loop.
    /// Returns false on failure.
    bool init_tls(const std::string& cert_path, const std::string& key_path);

    /// Reload TLS cert/key on SIGHUP. Returns false on failure (keeps old context).
    bool reload_tls(const std::string& cert_path, const std::string& key_path);

    /// Main accept loop coroutine. co_spawn this.
    asio::awaitable<void> accept_loop();

    /// Signal to stop accepting and close the acceptor.
    void stop();

    /// Update max connections on SIGHUP.
    void set_max_connections(uint32_t max);

    /// Current active connection count.
    size_t active_connections() const;

    /// Whether TLS is configured.
    bool is_tls_enabled() const;

private:
    /// Handle a new TCP connection: optional TLS handshake, then spawn HttpConnection.
    asio::awaitable<void> handle_new_connection(asio::ip::tcp::socket socket);

    asio::io_context& ioc_;
    HttpRouter& router_;
    TokenStore& token_store_;
    core::SubscriptionTracker& tracker_;
    core::UdsMultiplexer& uds_;
    asio::ip::tcp::acceptor acceptor_;
    const std::atomic<bool>& stopping_;

    // TLS context: shared_ptr + mutex for SIGHUP reload.
    mutable std::mutex tls_mutex_;
    std::shared_ptr<asio::ssl::context> tls_ctx_;

    std::atomic<uint32_t> max_connections_;
    std::atomic<uint32_t> active_connections_{0};

    static constexpr auto HANDSHAKE_TIMEOUT = std::chrono::seconds(5);
};

} // namespace chromatindb::relay::http
