#pragma once

#include <asio.hpp>
#include <asio/ssl.hpp>

#include <cstdint>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace chromatindb::relay::core {
class SubscriptionTracker;  // Forward declaration
class UdsMultiplexer;       // Forward declaration
} // namespace chromatindb::relay::core

namespace chromatindb::relay::http {

class HttpRouter;
class TokenStore;
struct HttpRequest;

/// Per-connection HTTP handler coroutine.
/// Handles one TCP/TLS connection with keep-alive request loop.
/// Uses get_if for TLS/plain branching (NOT std::visit -- ASAN safe per Phase 106 FIX-02).
/// Per D-35: one coroutine per connection.
class HttpConnection {
public:
    using TlsStream = asio::ssl::stream<asio::ip::tcp::socket>;
    using Stream = std::variant<asio::ip::tcp::socket, TlsStream>;

    HttpConnection(Stream stream, HttpRouter& router, TokenStore& token_store,
                   core::SubscriptionTracker& tracker, core::UdsMultiplexer& uds,
                   asio::io_context& ioc, uint32_t& active_connections);

    /// Main coroutine: keep-alive request loop.
    /// Reads headers, optional body, dispatches via router, sends response.
    asio::awaitable<void> handle();

private:
    /// Read bytes from stream (TLS or plain) using get_if branching.
    asio::awaitable<std::tuple<asio::error_code, size_t>> async_read_some(
        asio::mutable_buffer buf);

    /// Write bytes to stream (TLS or plain) using get_if branching.
    asio::awaitable<std::tuple<asio::error_code, size_t>> async_write(
        const std::string& data);

    /// Read until \r\n\r\n header end sentinel or buffer overflow.
    asio::awaitable<bool> read_headers(std::string& header_buf);

    /// Read exactly content_length bytes of body.
    asio::awaitable<bool> read_body(size_t content_length, std::vector<uint8_t>& body);

    /// Enter SSE streaming mode: create SseWriter, run drain loop, cleanup on close.
    asio::awaitable<void> run_sse_mode(uint64_t session_id);

    /// Shutdown the underlying socket cleanly.
    void shutdown_socket();

    Stream stream_;
    HttpRouter& router_;
    TokenStore& token_store_;
    core::SubscriptionTracker& tracker_;
    core::UdsMultiplexer& uds_;
    asio::io_context& ioc_;
    uint32_t& active_connections_;

    // Read buffer (persistent across reads within a connection)
    static constexpr size_t READ_BUF_SIZE = 8192;
    static constexpr size_t MAX_HEADER_SIZE = 16384;   // 16 KiB max headers
    static constexpr size_t MAX_BODY_SIZE = 110 * 1024 * 1024;  // 110 MiB max body

    // Carry-over buffer for data read past header boundary
    std::vector<uint8_t> pending_;
};

/// RAII guard that increments/decrements a connection counter.
/// Single-threaded model: plain uint32_t, no atomic needed.
class ConnectionGuard {
public:
    explicit ConnectionGuard(uint32_t& counter) : counter_(counter) {
        ++counter_;
    }
    ~ConnectionGuard() {
        --counter_;
    }
    ConnectionGuard(const ConnectionGuard&) = delete;
    ConnectionGuard& operator=(const ConnectionGuard&) = delete;

private:
    uint32_t& counter_;
};

} // namespace chromatindb::relay::http
