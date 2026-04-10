#include "relay/ws/ws_acceptor.h"
#include "relay/ws/ws_handshake.h"
#include "relay/ws/ws_session.h"

#include <spdlog/spdlog.h>

#include <array>
#include <string_view>

namespace chromatindb::relay::ws {

static constexpr auto use_nothrow = asio::as_tuple(asio::use_awaitable);

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

WsAcceptor::WsAcceptor(asio::io_context& ioc, SessionManager& manager,
                       const std::string& bind_address, uint16_t bind_port,
                       size_t max_send_queue, size_t max_connections,
                       core::Authenticator& authenticator,
                       core::UdsMultiplexer* uds_mux,
                       core::RequestRouter* router,
                       core::SubscriptionTracker* tracker)
    : acceptor_(ioc)
    , manager_(manager)
    , ioc_(ioc)
    , max_send_queue_(max_send_queue)
    , max_connections_(max_connections)
    , authenticator_(authenticator)
    , uds_mux_(uds_mux)
    , router_(router)
    , tracker_(tracker) {

    // Resolve bind address to determine protocol family (IPv4/IPv6).
    asio::ip::tcp::resolver resolver(ioc);
    auto endpoints = resolver.resolve(bind_address, std::to_string(bind_port));
    auto endpoint = endpoints.begin()->endpoint();

    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();
}

// ---------------------------------------------------------------------------
// TLS context management
// ---------------------------------------------------------------------------

bool WsAcceptor::init_tls(const std::string& cert_path, const std::string& key_path) {
    try {
        auto ctx = std::make_shared<asio::ssl::context>(
            asio::ssl::context::tlsv13_server);
        ctx->use_certificate_chain_file(cert_path);
        ctx->use_private_key_file(key_path, asio::ssl::context::pem);

        std::lock_guard<std::mutex> lock(tls_mutex_);
        tls_ctx_ = std::move(ctx);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("TLS init failed: {}", e.what());
        return false;
    }
}

bool WsAcceptor::reload_tls(const std::string& cert_path, const std::string& key_path) {
    try {
        auto ctx = std::make_shared<asio::ssl::context>(
            asio::ssl::context::tlsv13_server);
        ctx->use_certificate_chain_file(cert_path);
        ctx->use_private_key_file(key_path, asio::ssl::context::pem);

        std::lock_guard<std::mutex> lock(tls_mutex_);
        tls_ctx_ = std::move(ctx);
        spdlog::info("TLS context reloaded");
        return true;
    } catch (const std::exception& e) {
        spdlog::error("TLS reload failed: {}", e.what());
        return false;
    }
}

bool WsAcceptor::is_tls_enabled() const {
    std::lock_guard<std::mutex> lock(tls_mutex_);
    return tls_ctx_ != nullptr;
}

// ---------------------------------------------------------------------------
// Accept loop
// ---------------------------------------------------------------------------

asio::awaitable<void> WsAcceptor::accept_loop() {
    while (!stopping_) {
        auto [ec, socket] = co_await acceptor_.async_accept(use_nothrow);
        if (ec || stopping_) co_return;

        // Enforce configurable connection cap (per D-32).
        if (manager_.count() >= max_connections_) {
            spdlog::warn("connection rejected: max connections ({}) reached",
                        max_connections_);
            asio::error_code close_ec;
            socket.close(close_ec);
            continue;
        }

        // Disable Nagle for low-latency framed protocol.
        socket.set_option(asio::ip::tcp::no_delay(true));

        asio::co_spawn(ioc_, handle_new_connection(std::move(socket)),
                       asio::detached);
    }
}

void WsAcceptor::stop() {
    stopping_ = true;
    asio::error_code ec;
    acceptor_.close(ec);
}

void WsAcceptor::set_max_connections(size_t n) {
    auto old = max_connections_;
    max_connections_ = n;
    if (manager_.count() > n) {
        spdlog::warn("max_connections reduced to {} but {} active sessions (no mass disconnect)",
                    n, manager_.count());
    }
    if (old != n) {
        spdlog::info("max_connections updated: {} -> {}", old, n);
    }
}

// ---------------------------------------------------------------------------
// New connection handler
// ---------------------------------------------------------------------------

asio::awaitable<void> WsAcceptor::handle_new_connection(
    asio::ip::tcp::socket socket) {

    // 5-second handshake timeout (TLS + WS upgrade combined, per D-13).
    auto timer = std::make_shared<asio::steady_timer>(ioc_);
    timer->expires_after(HANDSHAKE_TIMEOUT);

    // Capture socket for timeout closure.
    auto socket_ptr = &socket;
    auto timeout_handler = [socket_ptr, timer](const asio::error_code& ec) {
        if (!ec) {
            // Timer expired -- kill the connection.
            asio::error_code close_ec;
            socket_ptr->lowest_layer().close(close_ec);
        }
    };
    timer->async_wait(timeout_handler);

    std::shared_ptr<asio::ssl::context> ctx;
    {
        std::lock_guard<std::mutex> lock(tls_mutex_);
        ctx = tls_ctx_;
    }

    if (ctx) {
        // WSS mode: TLS handshake then WS upgrade.
        try {
            asio::ssl::stream<asio::ip::tcp::socket> tls_stream(
                std::move(socket), *ctx);

            auto [hs_ec] = co_await tls_stream.async_handshake(
                asio::ssl::stream_base::server, use_nothrow);
            if (hs_ec) {
                spdlog::debug("TLS handshake failed: {}", hs_ec.message());
                timer->cancel();
                co_return;
            }

            timer->cancel();

            bool upgraded = co_await do_ws_upgrade(tls_stream);
            if (!upgraded) co_return;

            // Create WsSession with TLS stream variant.
            auto session = WsSession::create(
                WsSession::Stream(std::move(tls_stream)),
                manager_, ioc_.get_executor(), max_send_queue_,
                authenticator_, ioc_, uds_mux_, router_, tracker_);
            auto id = manager_.add_session(session);
            spdlog::info("session {}: WebSocket connection established (WSS)", id);
            session->start(id);
        } catch (const std::exception& e) {
            spdlog::debug("TLS connection error: {}", e.what());
            timer->cancel();
        }
    } else {
        // Plain WS mode.
        timer->cancel();

        bool upgraded = co_await do_ws_upgrade(socket);
        if (!upgraded) co_return;

        // Create WsSession with plain TCP socket variant.
        auto session = WsSession::create(
            WsSession::Stream(std::move(socket)),
            manager_, ioc_.get_executor(), max_send_queue_,
            authenticator_, ioc_, uds_mux_, router_, tracker_);
        auto id = manager_.add_session(session);
        spdlog::info("session {}: WebSocket connection established (WS)", id);
        session->start(id);
    }
}

// ---------------------------------------------------------------------------
// WebSocket upgrade handshake (template -- works for both stream types)
// ---------------------------------------------------------------------------

template<typename Stream>
asio::awaitable<bool> WsAcceptor::do_ws_upgrade(Stream& stream) {
    // Read HTTP upgrade request (up to 4096 bytes).
    std::array<uint8_t, 4096> buf{};
    size_t total = 0;
    static constexpr std::string_view END_MARKER = "\r\n\r\n";

    while (total < buf.size()) {
        auto [ec, n] = co_await stream.async_read_some(
            asio::buffer(buf.data() + total, buf.size() - total), use_nothrow);
        if (ec || n == 0) co_return false;
        total += n;

        // Check if we have the full HTTP request.
        std::string_view data(reinterpret_cast<const char*>(buf.data()), total);
        if (data.find(END_MARKER) != std::string_view::npos) break;
    }

    std::string_view raw(reinterpret_cast<const char*>(buf.data()), total);
    auto result = parse_upgrade_request(raw);

    if (!result.is_upgrade) {
        // Not a WebSocket upgrade request -- send 426 (per D-12).
        auto response = build_426_response();
        co_await asio::async_write(stream, asio::buffer(response), use_nothrow);
        co_return false;
    }

    if (!result.request) {
        // Upgrade attempt but invalid.
        spdlog::warn("invalid WS upgrade request: {}", result.error);
        co_return false;
    }

    // Complete the upgrade.
    auto accept_key = compute_accept_key(result.request->websocket_key);
    auto response = build_upgrade_response(accept_key);
    auto [ec, n] = co_await asio::async_write(
        stream, asio::buffer(response), use_nothrow);
    if (ec) co_return false;

    co_return true;
}

// Explicit template instantiations for the two stream types.
template asio::awaitable<bool> WsAcceptor::do_ws_upgrade<asio::ip::tcp::socket>(
    asio::ip::tcp::socket&);
template asio::awaitable<bool> WsAcceptor::do_ws_upgrade<asio::ssl::stream<asio::ip::tcp::socket>>(
    asio::ssl::stream<asio::ip::tcp::socket>&);

} // namespace chromatindb::relay::ws
