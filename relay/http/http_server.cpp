#include "relay/http/http_server.h"
#include "relay/http/http_connection.h"
#include "relay/http/http_router.h"
#include "relay/http/token_store.h"

#include <spdlog/spdlog.h>

namespace chromatindb::relay::http {

static constexpr auto use_nothrow = asio::as_tuple(asio::use_awaitable);

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

HttpServer::HttpServer(asio::io_context& ioc, HttpRouter& router, TokenStore& token_store,
                       core::SubscriptionTracker& tracker, core::UdsMultiplexer& uds,
                       const std::string& bind_address, uint16_t bind_port,
                       uint32_t max_connections, const std::atomic<bool>& stopping)
    : ioc_(ioc)
    , router_(router)
    , token_store_(token_store)
    , tracker_(tracker)
    , uds_(uds)
    , acceptor_(ioc)
    , stopping_(stopping)
    , max_connections_(max_connections) {

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

bool HttpServer::init_tls(const std::string& cert_path, const std::string& key_path) {
    try {
        auto ctx = std::make_shared<asio::ssl::context>(
            asio::ssl::context::tlsv13_server);
        ctx->use_certificate_chain_file(cert_path);
        ctx->use_private_key_file(key_path, asio::ssl::context::pem);

        std::lock_guard<std::mutex> lock(tls_mutex_);
        tls_ctx_ = std::move(ctx);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("HTTP TLS init failed: {}", e.what());
        return false;
    }
}

bool HttpServer::reload_tls(const std::string& cert_path, const std::string& key_path) {
    try {
        auto ctx = std::make_shared<asio::ssl::context>(
            asio::ssl::context::tlsv13_server);
        ctx->use_certificate_chain_file(cert_path);
        ctx->use_private_key_file(key_path, asio::ssl::context::pem);

        std::lock_guard<std::mutex> lock(tls_mutex_);
        tls_ctx_ = std::move(ctx);
        spdlog::info("HTTP TLS context reloaded");
        return true;
    } catch (const std::exception& e) {
        spdlog::error("HTTP TLS reload failed: {}", e.what());
        return false;
    }
}

bool HttpServer::is_tls_enabled() const {
    std::lock_guard<std::mutex> lock(tls_mutex_);
    return tls_ctx_ != nullptr;
}

// ---------------------------------------------------------------------------
// Accept loop
// ---------------------------------------------------------------------------

asio::awaitable<void> HttpServer::accept_loop() {
    spdlog::info("HTTP server accept loop started");
    while (!stopping_.load(std::memory_order_relaxed)) {
        auto [ec, socket] = co_await acceptor_.async_accept(use_nothrow);
        if (ec || stopping_.load(std::memory_order_relaxed)) {
            if (ec && ec != asio::error::operation_aborted) {
                spdlog::error("HTTP accept error: {}", ec.message());
            }
            co_return;
        }

        // Enforce connection cap.
        if (active_connections_.load(std::memory_order_relaxed) >=
            max_connections_.load(std::memory_order_relaxed)) {
            spdlog::warn("HTTP connection rejected: max connections ({}) reached",
                        max_connections_.load(std::memory_order_relaxed));
            asio::error_code close_ec;
            socket.close(close_ec);
            continue;
        }

        // Disable Nagle for low-latency request/response.
        socket.set_option(asio::ip::tcp::no_delay(true));

        asio::co_spawn(ioc_, handle_new_connection(std::move(socket)), asio::detached);
    }
}

void HttpServer::stop() {
    asio::error_code ec;
    acceptor_.close(ec);
    spdlog::info("HTTP server accept loop stopped");
}

void HttpServer::set_max_connections(uint32_t max) {
    auto old = max_connections_.load(std::memory_order_relaxed);
    max_connections_.store(max, std::memory_order_relaxed);
    if (old != max) {
        spdlog::info("HTTP max_connections updated: {} -> {}", old, max);
    }
}

size_t HttpServer::active_connections() const {
    return active_connections_.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// New connection handler
// ---------------------------------------------------------------------------

asio::awaitable<void> HttpServer::handle_new_connection(asio::ip::tcp::socket socket) {
    std::shared_ptr<asio::ssl::context> ctx;
    {
        std::lock_guard<std::mutex> lock(tls_mutex_);
        ctx = tls_ctx_;
    }

    if (ctx) {
        // HTTPS mode: TLS handshake with timeout.
        try {
            // 5-second handshake timeout.
            auto timer = std::make_shared<asio::steady_timer>(ioc_);
            timer->expires_after(HANDSHAKE_TIMEOUT);

            asio::ssl::stream<asio::ip::tcp::socket> tls_stream(
                std::move(socket), *ctx);

            auto* lowest = &tls_stream.lowest_layer();
            timer->async_wait([lowest](const asio::error_code& ec) {
                if (!ec) {
                    asio::error_code close_ec;
                    lowest->close(close_ec);
                }
            });

            auto [hs_ec] = co_await tls_stream.async_handshake(
                asio::ssl::stream_base::server, use_nothrow);
            timer->cancel();

            if (hs_ec) {
                spdlog::debug("HTTP TLS handshake failed: {}", hs_ec.message());
                co_return;
            }

            HttpConnection conn(
                HttpConnection::Stream(std::move(tls_stream)),
                router_, token_store_, tracker_, uds_, ioc_, active_connections_);
            co_await conn.handle();
        } catch (const std::exception& e) {
            spdlog::debug("HTTP TLS connection error: {}", e.what());
        }
    } else {
        // Plain HTTP mode.
        HttpConnection conn(
            HttpConnection::Stream(std::move(socket)),
            router_, token_store_, tracker_, uds_, ioc_, active_connections_);
        co_await conn.handle();
    }
}

} // namespace chromatindb::relay::http
