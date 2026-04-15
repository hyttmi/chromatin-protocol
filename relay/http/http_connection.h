#pragma once

#include <asio.hpp>
#include <asio/ssl.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace chromatindb::relay::core {
class SubscriptionTracker;  // Forward declaration
class UdsMultiplexer;       // Forward declaration
} // namespace chromatindb::relay::core

namespace chromatindb::relay::http {

class DataHandlers;  // Forward declaration for streaming dispatch
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

    /// Set DataHandlers for streaming dispatch (upload/download of large blobs).
    /// Called once after construction by relay_main or http_server.
    void set_data_handlers(DataHandlers* handlers) { data_handlers_ = handlers; }

    // --- Public streaming API (used by DataHandlers for large blob I/O) ---

    /// Read exactly content_length bytes of body.
    asio::awaitable<bool> read_body(size_t content_length, std::vector<uint8_t>& body);

    /// Read HTTP body incrementally in chunks of chunk_size bytes.
    /// Calls chunk_cb for each chunk read. Final chunk may be smaller.
    /// Returns false on read error.
    using ChunkCallback = std::function<asio::awaitable<bool>(std::span<const uint8_t>)>;
    asio::awaitable<bool> read_body_chunked(size_t content_length, size_t chunk_size,
                                             ChunkCallback chunk_cb);

    /// Start a chunked transfer encoding response.
    asio::awaitable<bool> write_chunked_te_start(uint16_t status, std::string_view content_type);

    /// Write one HTTP chunk in chunked-TE format.
    asio::awaitable<bool> write_chunked_te_chunk(std::span<const uint8_t> data);

    /// Write the terminating chunk to end chunked transfer encoding.
    asio::awaitable<bool> write_chunked_te_end();

private:
    /// Read bytes from stream (TLS or plain) using get_if branching.
    asio::awaitable<std::tuple<asio::error_code, size_t>> async_read_some(
        asio::mutable_buffer buf);

    /// Write bytes to stream (TLS or plain) using get_if branching.
    asio::awaitable<std::tuple<asio::error_code, size_t>> async_write(
        const std::string& data);

    /// Read until \r\n\r\n header end sentinel or buffer overflow.
    asio::awaitable<bool> read_headers(std::string& header_buf);

    /// Enter SSE streaming mode: create SseWriter, run drain loop, cleanup on close.
    asio::awaitable<void> run_sse_mode(uint64_t session_id);

    /// Write a buffer sequence to the stream (scatter-gather).
    /// Handles TLS/plain branching via get_if.
    template<typename BufferSequence>
    asio::awaitable<std::tuple<asio::error_code, size_t>> async_write_buffers(
        const BufferSequence& bufs) {
        // Defined in same use_nothrow pattern as async_write.
        static constexpr auto use_nothrow = asio::as_tuple(asio::use_awaitable);
        if (auto* tls = std::get_if<TlsStream>(&stream_)) {
            co_return co_await asio::async_write(*tls, bufs, use_nothrow);
        }
        auto* tcp = std::get_if<asio::ip::tcp::socket>(&stream_);
        co_return co_await asio::async_write(*tcp, bufs, use_nothrow);
    }

    /// Shutdown the underlying socket cleanly.
    void shutdown_socket();

    Stream stream_;
    HttpRouter& router_;
    TokenStore& token_store_;
    core::SubscriptionTracker& tracker_;
    core::UdsMultiplexer& uds_;
    asio::io_context& ioc_;
    uint32_t& active_connections_;
    DataHandlers* data_handlers_ = nullptr;

    // Read buffer (persistent across reads within a connection)
    static constexpr size_t READ_BUF_SIZE = 8192;
    static constexpr size_t MAX_HEADER_SIZE = 16384;   // 16 KiB max headers
    static constexpr size_t MAX_BODY_SIZE = 510 * 1024 * 1024;  // 510 MiB max body (500 MiB blob + 10 MiB overhead)

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
