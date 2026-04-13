#pragma once

#include <asio.hpp>
#include <asio/ssl.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

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
                   asio::io_context& ioc, std::atomic<uint32_t>& active_connections);

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

    /// Shutdown the underlying socket cleanly.
    void shutdown_socket();

    Stream stream_;
    HttpRouter& router_;
    TokenStore& token_store_;
    asio::io_context& ioc_;
    std::atomic<uint32_t>& active_connections_;

    // Read buffer (persistent across reads within a connection)
    static constexpr size_t READ_BUF_SIZE = 8192;
    static constexpr size_t MAX_HEADER_SIZE = 16384;   // 16 KiB max headers
    static constexpr size_t MAX_BODY_SIZE = 110 * 1024 * 1024;  // 110 MiB max body

    // Carry-over buffer for data read past header boundary
    std::vector<uint8_t> pending_;
};

/// RAII guard that decrements an atomic counter on destruction.
class ConnectionGuard {
public:
    explicit ConnectionGuard(std::atomic<uint32_t>& counter) : counter_(counter) {
        counter_.fetch_add(1, std::memory_order_relaxed);
    }
    ~ConnectionGuard() {
        counter_.fetch_sub(1, std::memory_order_relaxed);
    }
    ConnectionGuard(const ConnectionGuard&) = delete;
    ConnectionGuard& operator=(const ConnectionGuard&) = delete;

private:
    std::atomic<uint32_t>& counter_;
};

} // namespace chromatindb::relay::http
