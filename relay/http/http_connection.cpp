#include "relay/http/http_connection.h"
#include "relay/http/handlers_data.h"
#include "relay/http/http_parser.h"
#include "relay/http/http_response.h"
#include "relay/http/http_router.h"
#include "relay/http/sse_writer.h"
#include "relay/http/token_store.h"
#include "relay/core/chunked_stream.h"
#include "relay/core/subscription_tracker.h"
#include "relay/core/uds_multiplexer.h"
#include "relay/util/endian.h"
#include "relay/wire/transport_codec.h"
#include "relay/wire/transport_generated.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cstdio>
#include <cstring>

namespace chromatindb::relay::http {

static constexpr auto use_nothrow = asio::as_tuple(asio::use_awaitable);

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

HttpConnection::HttpConnection(Stream stream, HttpRouter& router, TokenStore& token_store,
                               core::SubscriptionTracker& tracker, core::UdsMultiplexer& uds,
                               asio::io_context& ioc, uint32_t& active_connections)
    : stream_(std::move(stream))
    , router_(router)
    , token_store_(token_store)
    , tracker_(tracker)
    , uds_(uds)
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
// Chunked body reading (incremental)
// ---------------------------------------------------------------------------

asio::awaitable<bool> HttpConnection::read_body_chunked(
    size_t content_length, size_t chunk_size, ChunkCallback chunk_cb) {

    size_t total_read = 0;
    std::vector<uint8_t> chunk_buf;
    chunk_buf.reserve(chunk_size);

    // First consume any pending bytes from header read
    if (!pending_.empty()) {
        size_t to_take = std::min(pending_.size(), content_length);
        chunk_buf.insert(chunk_buf.end(), pending_.begin(),
                        pending_.begin() + static_cast<std::ptrdiff_t>(to_take));
        if (to_take < pending_.size()) {
            pending_.erase(pending_.begin(),
                          pending_.begin() + static_cast<std::ptrdiff_t>(to_take));
        } else {
            pending_.clear();
        }
        total_read += chunk_buf.size();

        // Emit full chunks
        while (chunk_buf.size() >= chunk_size) {
            std::span<const uint8_t> chunk_span(chunk_buf.data(), chunk_size);
            if (!co_await chunk_cb(chunk_span)) co_return false;
            chunk_buf.erase(chunk_buf.begin(),
                           chunk_buf.begin() + static_cast<std::ptrdiff_t>(chunk_size));
        }
    }

    std::array<uint8_t, READ_BUF_SIZE> read_buf{};
    while (total_read < content_length) {
        size_t want = std::min(static_cast<size_t>(READ_BUF_SIZE),
                               content_length - total_read);
        auto [ec, n] = co_await async_read_some(asio::buffer(read_buf.data(), want));
        if (ec || n == 0) co_return false;

        chunk_buf.insert(chunk_buf.end(), read_buf.data(), read_buf.data() + n);
        total_read += n;

        // Emit full chunks
        while (chunk_buf.size() >= chunk_size) {
            std::span<const uint8_t> chunk_span(chunk_buf.data(), chunk_size);
            if (!co_await chunk_cb(chunk_span)) co_return false;
            chunk_buf.erase(chunk_buf.begin(),
                           chunk_buf.begin() + static_cast<std::ptrdiff_t>(chunk_size));
        }
    }

    // Emit final partial chunk if any
    if (!chunk_buf.empty()) {
        if (!co_await chunk_cb(chunk_buf)) co_return false;
    }

    co_return true;
}

// ---------------------------------------------------------------------------
// HTTP chunked transfer encoding writer
// ---------------------------------------------------------------------------

asio::awaitable<bool> HttpConnection::write_chunked_te_start(
    uint16_t status, std::string_view content_type) {
    HttpResponse resp;
    resp.status = status;
    resp.status_text = HttpResponse::status_text_for_public(status);
    resp.headers.emplace_back("Content-Type", std::string(content_type));
    resp.headers.emplace_back("Transfer-Encoding", "chunked");
    auto header_str = resp.serialize_header();
    auto [ec, _] = co_await async_write(header_str);
    co_return !ec;
}

asio::awaitable<bool> HttpConnection::write_chunked_te_chunk(
    std::span<const uint8_t> data) {
    // Format: "{hex_size}\r\n{data}\r\n"
    char hex_buf[32];
    int hex_len = std::snprintf(hex_buf, sizeof(hex_buf), "%zx\r\n", data.size());
    static constexpr std::string_view CRLF = "\r\n";

    std::array<asio::const_buffer, 3> bufs = {
        asio::buffer(hex_buf, static_cast<size_t>(hex_len)),
        asio::buffer(data.data(), data.size()),
        asio::buffer(CRLF.data(), CRLF.size())
    };
    auto [ec, _] = co_await async_write_buffers(bufs);
    co_return !ec;
}

asio::awaitable<bool> HttpConnection::write_chunked_te_end() {
    static constexpr std::string_view TERMINATOR = "0\r\n\r\n";
    auto [ec, _] = co_await async_write(std::string(TERMINATOR));
    co_return !ec;
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
            auto header_str = resp.serialize_header();
            std::array<asio::const_buffer, 2> bufs = {
                asio::buffer(header_str),
                asio::buffer(resp.body.data(), resp.body.size())
            };
            auto [wec, _] = co_await async_write_buffers(bufs);
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
            auto resp = HttpResponse::error(413, "payload_too_large", "body exceeds 510 MiB limit");
            auto header_str = resp.serialize_header();
            std::array<asio::const_buffer, 2> bufs = {
                asio::buffer(header_str),
                asio::buffer(resp.body.data(), resp.body.size())
            };
            auto [wec, _] = co_await async_write_buffers(bufs);
            shutdown_socket();
            co_return;
        }

        // 3b. Streaming upload check: POST /blob with body >= STREAMING_THRESHOLD.
        // Must be checked before read_body() to avoid buffering the full body.
        if (data_handlers_ && req.content_length >= core::STREAMING_THRESHOLD &&
            req.method == "POST" && req.path == "/blob") {

            // Inline auth check (same logic as HttpRouter uses)
            HttpSessionState* session = nullptr;
            auto auth_it = req.headers.find("authorization");
            if (auth_it != req.headers.end()) {
                auto token = auth_it->second;
                if (token.size() > 7 && token.compare(0, 7, "Bearer ") == 0) {
                    token = token.substr(7);
                }
                session = token_store_.lookup(token);
            }

            if (!session) {
                // Must drain body to keep connection clean for keep-alive
                std::vector<uint8_t> discard;
                co_await read_body(req.content_length, discard);
                auto resp = HttpResponse::error(401, "unauthorized", "bearer token required");
                auto hdr = resp.serialize_header();
                std::array<asio::const_buffer, 2> bufs = {
                    asio::buffer(hdr),
                    asio::buffer(resp.body.data(), resp.body.size())
                };
                auto [wec, _] = co_await async_write_buffers(bufs);
                if (wec) { shutdown_socket(); co_return; }
                if (!req.keep_alive) { shutdown_socket(); co_return; }
                continue;  // Next request in keep-alive loop
            }

            if (!session->rate_limiter.try_consume()) {
                std::vector<uint8_t> discard;
                co_await read_body(req.content_length, discard);
                auto resp = HttpResponse::error(429, "rate_limited", "rate limit exceeded");
                auto hdr = resp.serialize_header();
                std::array<asio::const_buffer, 2> bufs = {
                    asio::buffer(hdr),
                    asio::buffer(resp.body.data(), resp.body.size())
                };
                auto [wec, _] = co_await async_write_buffers(bufs);
                if (wec) { shutdown_socket(); co_return; }
                if (!req.keep_alive) { shutdown_socket(); co_return; }
                continue;
            }

            // Dispatch to streaming write handler (handler reads body incrementally)
            auto response = co_await data_handlers_->handle_blob_write_streaming(
                req, *this, session);
            auto hdr = response.serialize_header();
            if (response.body.empty()) {
                auto [wec, _] = co_await async_write(hdr);
                if (wec) { shutdown_socket(); co_return; }
            } else {
                std::array<asio::const_buffer, 2> bufs = {
                    asio::buffer(hdr),
                    asio::buffer(response.body.data(), response.body.size())
                };
                auto [wec, _] = co_await async_write_buffers(bufs);
                if (wec) { shutdown_socket(); co_return; }
            }
            if (!req.keep_alive) { shutdown_socket(); co_return; }
            continue;  // Next request in keep-alive loop
        }

        // 3c. Streaming blob read check: GET /blob/{ns}/{hash} with data_handlers_ set.
        if (data_handlers_ && req.method == "GET" &&
            req.path.starts_with("/blob/") && req.path.size() > 6) {

            // Inline auth check (same as upload path)
            HttpSessionState* session = nullptr;
            auto auth_it = req.headers.find("authorization");
            if (auth_it != req.headers.end()) {
                auto token = auth_it->second;
                if (token.size() > 7 && token.compare(0, 7, "Bearer ") == 0) {
                    token = token.substr(7);
                }
                session = token_store_.lookup(token);
            }
            if (!session) {
                auto resp = HttpResponse::error(401, "unauthorized", "bearer token required");
                auto hdr = resp.serialize_header();
                std::array<asio::const_buffer, 2> bufs = {
                    asio::buffer(hdr), asio::buffer(resp.body.data(), resp.body.size())
                };
                auto [wec, _] = co_await async_write_buffers(bufs);
                if (wec) { shutdown_socket(); co_return; }
                if (!req.keep_alive) { shutdown_socket(); co_return; }
                continue;
            }

            if (!session->rate_limiter.try_consume()) {
                auto resp = HttpResponse::error(429, "rate_limited", "rate limit exceeded");
                auto hdr = resp.serialize_header();
                std::array<asio::const_buffer, 2> bufs = {
                    asio::buffer(hdr), asio::buffer(resp.body.data(), resp.body.size())
                };
                auto [wec, _] = co_await async_write_buffers(bufs);
                if (wec) { shutdown_socket(); co_return; }
                if (!req.keep_alive) { shutdown_socket(); co_return; }
                continue;
            }

            auto response = co_await data_handlers_->handle_blob_read_streaming(
                req, *this, session);
            if (response) {
                // Error response or small blob -- send normally
                auto hdr = response->serialize_header();
                if (response->body.empty()) {
                    auto [wec, _] = co_await async_write(hdr);
                    if (wec) { shutdown_socket(); co_return; }
                } else {
                    std::array<asio::const_buffer, 2> bufs = {
                        asio::buffer(hdr),
                        asio::buffer(response->body.data(), response->body.size())
                    };
                    auto [wec, _] = co_await async_write_buffers(bufs);
                    if (wec) { shutdown_socket(); co_return; }
                }
            }
            // If nullopt, response already written via chunked-TE
            if (!req.keep_alive) { shutdown_socket(); co_return; }
            continue;
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

        // 5. Dispatch to router (async to support coroutine query handlers).
        auto response = co_await router_.dispatch_async(req, body, token_store_);

        // 5b. Check for SSE mode signal (X-SSE-Session-Id header from /events handler).
        std::string sse_session_id_str;
        for (const auto& [key, val] : response.headers) {
            if (key == "X-SSE-Session-Id") {
                sse_session_id_str = val;
                break;
            }
        }

        if (!sse_session_id_str.empty()) {
            uint64_t sse_sid = std::stoull(sse_session_id_str);
            // Send the SSE response headers (no body, connection stays open).
            auto header_str = response.serialize_header();
            auto [wec, _] = co_await async_write(header_str);
            if (wec) {
                shutdown_socket();
                co_return;
            }
            // Enter SSE mode -- this blocks until the SseWriter closes.
            co_await run_sse_mode(sse_sid);
            shutdown_socket();
            co_return;
        }

        // 6. Serialize and send response (scatter-gather: header + body).
        auto header_str = response.serialize_header();
        if (response.body.empty()) {
            // Header-only response (204 No Content, etc.)
            auto [wec, _] = co_await async_write(header_str);
            if (wec) { shutdown_socket(); co_return; }
        } else {
            // Scatter-gather: header + body in one async_write call
            std::array<asio::const_buffer, 2> bufs = {
                asio::buffer(header_str),
                asio::buffer(response.body.data(), response.body.size())
            };
            auto [wec, _] = co_await async_write_buffers(bufs);
            if (wec) { shutdown_socket(); co_return; }
        }

        // 7. Check keep-alive.
        if (!req.keep_alive) {
            shutdown_socket();
            co_return;
        }
    }
}

// ---------------------------------------------------------------------------
// SSE streaming mode
// ---------------------------------------------------------------------------

asio::awaitable<void> HttpConnection::run_sse_mode(uint64_t session_id) {
    // Look up session in token_store.
    auto* session = token_store_.lookup_by_id(session_id);
    if (!session) {
        co_return;  // Session gone already
    }

    // Create WriteFn that writes to the stream (TLS or plain).
    SseWriter::WriteFn write_fn = [this](std::string_view data) -> asio::awaitable<bool> {
        std::string s(data);
        auto [ec, n] = co_await async_write(s);
        co_return !ec;
    };

    // Create SseWriter on the coroutine stack.
    SseWriter writer(ioc_.get_executor(), std::move(write_fn));

    // Set the sse_writer pointer on the session so notifications can be pushed.
    session->sse_writer = &writer;

    spdlog::info("SSE stream started for session {}", session_id);

    // Run the drain loop -- blocks until writer is closed.
    co_await writer.run();

    spdlog::info("SSE stream ended for session {}", session_id);

    // Cleanup: clear sse_writer pointer.
    // Re-lookup session (it may have been reaped during the long SSE session).
    auto* s = token_store_.lookup_by_id(session_id);
    if (s) {
        s->sse_writer = nullptr;
    }

    // Cleanup subscriptions (SSE close path).
    auto empty_namespaces = tracker_.remove_client(session_id);
    if (!empty_namespaces.empty() && uds_.is_connected()) {
        // Forward Unsubscribe to node for namespaces that dropped to 0 subscribers.
        std::vector<uint8_t> payload;
        payload.reserve(2 + empty_namespaces.size() * 32);
        uint8_t count_buf[2];
        util::store_u16_be(count_buf, static_cast<uint16_t>(empty_namespaces.size()));
        payload.insert(payload.end(), count_buf, count_buf + 2);
        for (const auto& ns : empty_namespaces) {
            payload.insert(payload.end(), ns.begin(), ns.end());
        }
        auto msg = wire::TransportCodec::encode(
            chromatindb::wire::TransportMsgType_Unsubscribe, payload, 0);
        uds_.send(std::move(msg));
        spdlog::debug("SSE close: forwarded {} unsubscribe(s) to node for session {}",
                      empty_namespaces.size(), session_id);
    }
}

} // namespace chromatindb::relay::http
