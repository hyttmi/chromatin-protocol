#include "relay/http/http_connection.h"
#include "relay/http/http_parser.h"
#include "relay/http/http_response.h"
#include "relay/http/http_router.h"
#include "relay/http/token_store.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cstring>

namespace chromatindb::relay::http {

static constexpr auto use_nothrow = asio::as_tuple(asio::use_awaitable);

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

HttpConnection::HttpConnection(Stream stream, HttpRouter& router, TokenStore& token_store,
                               asio::io_context& ioc, std::atomic<uint32_t>& active_connections)
    : stream_(std::move(stream))
    , router_(router)
    , token_store_(token_store)
    , ioc_(ioc)
    , active_connections_(active_connections) {}

// ---------------------------------------------------------------------------
// TLS/plain dual-mode I/O (get_if branching -- NOT std::visit)
// ---------------------------------------------------------------------------

asio::awaitable<std::tuple<asio::error_code, size_t>> HttpConnection::async_read_some(
    asio::mutable_buffer buf) {
    if (auto* tls = std::get_if<TlsStream>(&stream_)) {
        co_return co_await tls->async_read_some(buf, use_nothrow);
    }
    auto* tcp = std::get_if<asio::ip::tcp::socket>(&stream_);
    co_return co_await tcp->async_read_some(buf, use_nothrow);
}

asio::awaitable<std::tuple<asio::error_code, size_t>> HttpConnection::async_write(
    const std::string& data) {
    if (auto* tls = std::get_if<TlsStream>(&stream_)) {
        co_return co_await asio::async_write(*tls, asio::buffer(data), use_nothrow);
    }
    auto* tcp = std::get_if<asio::ip::tcp::socket>(&stream_);
    co_return co_await asio::async_write(*tcp, asio::buffer(data), use_nothrow);
}

// ---------------------------------------------------------------------------
// Header reading
// ---------------------------------------------------------------------------

asio::awaitable<bool> HttpConnection::read_headers(std::string& header_buf) {
    // First check if pending_ already contains headers from previous read.
    if (!pending_.empty()) {
        header_buf.append(reinterpret_cast<const char*>(pending_.data()), pending_.size());
        pending_.clear();
        auto pos = header_buf.find("\r\n\r\n");
        if (pos != std::string::npos) {
            // Save any bytes beyond the header end for body reading.
            size_t header_end = pos + 4;
            if (header_end < header_buf.size()) {
                pending_.assign(header_buf.begin() + static_cast<std::ptrdiff_t>(header_end),
                               header_buf.end());
                header_buf.resize(header_end);
            }
            co_return true;
        }
    }

    std::array<uint8_t, READ_BUF_SIZE> buf{};

    while (header_buf.size() < MAX_HEADER_SIZE) {
        auto [ec, n] = co_await async_read_some(asio::buffer(buf));
        if (ec || n == 0) co_return false;

        header_buf.append(reinterpret_cast<const char*>(buf.data()), n);

        if (header_buf.size() > MAX_HEADER_SIZE) co_return false;

        auto pos = header_buf.find("\r\n\r\n");
        if (pos != std::string::npos) {
            // Save any bytes beyond the header end for body reading.
            size_t header_end = pos + 4;
            if (header_end < header_buf.size()) {
                pending_.assign(header_buf.begin() + static_cast<std::ptrdiff_t>(header_end),
                               header_buf.end());
                header_buf.resize(header_end);
            }
            co_return true;
        }
    }
    co_return false;  // Header too large
}

// ---------------------------------------------------------------------------
// Body reading
// ---------------------------------------------------------------------------

asio::awaitable<bool> HttpConnection::read_body(size_t content_length, std::vector<uint8_t>& body) {
    body.reserve(content_length);

    // First consume any pending bytes from header read.
    if (!pending_.empty()) {
        size_t to_take = std::min(pending_.size(), content_length);
        body.insert(body.end(), pending_.begin(),
                    pending_.begin() + static_cast<std::ptrdiff_t>(to_take));
        if (to_take < pending_.size()) {
            pending_.erase(pending_.begin(),
                          pending_.begin() + static_cast<std::ptrdiff_t>(to_take));
        } else {
            pending_.clear();
        }
    }

    std::array<uint8_t, READ_BUF_SIZE> buf{};
    while (body.size() < content_length) {
        size_t want = std::min(static_cast<size_t>(READ_BUF_SIZE), content_length - body.size());
        auto [ec, n] = co_await async_read_some(asio::buffer(buf.data(), want));
        if (ec || n == 0) co_return false;
        body.insert(body.end(), buf.data(), buf.data() + n);
    }

    co_return true;
}

// ---------------------------------------------------------------------------
// Socket shutdown
// ---------------------------------------------------------------------------

void HttpConnection::shutdown_socket() {
    if (auto* tls = std::get_if<TlsStream>(&stream_)) {
        asio::error_code ec;
        tls->lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        tls->lowest_layer().close(ec);
    } else if (auto* tcp = std::get_if<asio::ip::tcp::socket>(&stream_)) {
        asio::error_code ec;
        tcp->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        tcp->close(ec);
    }
}

// ---------------------------------------------------------------------------
// Main coroutine: keep-alive request loop
// ---------------------------------------------------------------------------

asio::awaitable<void> HttpConnection::handle() {
    ConnectionGuard guard(active_connections_);

    // Keep-alive loop: process multiple sequential requests on this connection.
    while (true) {
        // 1. Read HTTP request headers until \r\n\r\n.
        std::string header_buf;
        header_buf.reserve(4096);
        bool ok = co_await read_headers(header_buf);
        if (!ok) {
            shutdown_socket();
            co_return;
        }

        // 2. Parse request line and headers.
        HttpRequest req;
        auto lines_start = std::string_view(header_buf);

        // Parse request line (first line).
        auto first_end = lines_start.find("\r\n");
        if (first_end == std::string_view::npos) {
            shutdown_socket();
            co_return;
        }
        if (!parse_request_line(lines_start.substr(0, first_end), req)) {
            // Malformed request line -- send 400.
            auto resp = HttpResponse::error(400, "bad_request", "malformed request line");
            auto [wec, _] = co_await async_write(resp.serialize());
            shutdown_socket();
            co_return;
        }

        // Parse headers (remaining lines).
        auto remaining = lines_start.substr(first_end + 2);
        while (!remaining.empty()) {
            auto line_end = remaining.find("\r\n");
            if (line_end == std::string_view::npos) break;
            auto line = remaining.substr(0, line_end);
            if (line.empty()) break;  // End of headers
            parse_header_line(line, req);
            remaining = remaining.substr(line_end + 2);
        }
        finalize_headers(req);

        // 3. If Content-Length > max body, respond 413 immediately.
        if (req.content_length > MAX_BODY_SIZE) {
            auto resp = HttpResponse::error(413, "payload_too_large", "body exceeds 110 MiB limit");
            auto [wec, _] = co_await async_write(resp.serialize());
            shutdown_socket();
            co_return;
        }

        // 4. Read body if Content-Length > 0.
        std::vector<uint8_t> body;
        if (req.content_length > 0) {
            bool body_ok = co_await read_body(req.content_length, body);
            if (!body_ok) {
                shutdown_socket();
                co_return;
            }
        }

        // 5. Dispatch to router.
        auto response = router_.dispatch(req, body, token_store_);

        // 6. Serialize and send response.
        auto serialized = response.serialize();
        auto [wec, _] = co_await async_write(serialized);
        if (wec) {
            shutdown_socket();
            co_return;
        }

        // 7. Check keep-alive.
        if (!req.keep_alive) {
            shutdown_socket();
            co_return;
        }
    }
}

} // namespace chromatindb::relay::http
