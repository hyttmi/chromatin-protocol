#include "relay/core/metrics_collector.h"

#include <spdlog/spdlog.h>

#include <array>
#include <string>

namespace chromatindb::relay::core {

MetricsCollector::MetricsCollector(asio::io_context& ioc,
                                   const std::string& metrics_bind,
                                   const std::atomic<bool>& stopping)
    : ioc_(ioc)
    , stopping_(stopping)
    , start_time_(std::chrono::steady_clock::now())
    , metrics_bind_(metrics_bind) {
}

// =============================================================================
// Lifecycle
// =============================================================================

void MetricsCollector::start() {
    if (metrics_bind_.empty()) return;
    try {
        auto colon = metrics_bind_.rfind(':');
        if (colon == std::string::npos) {
            spdlog::error("metrics: invalid bind address '{}' (missing ':')", metrics_bind_);
            return;
        }
        auto host = metrics_bind_.substr(0, colon);
        auto port = metrics_bind_.substr(colon + 1);

        asio::ip::tcp::resolver resolver(ioc_);
        auto endpoints = resolver.resolve(host, port);
        if (endpoints.empty()) {
            spdlog::error("metrics: could not resolve '{}'", metrics_bind_);
            return;
        }

        auto endpoint = endpoints.begin()->endpoint();
        acceptor_ = std::make_unique<asio::ip::tcp::acceptor>(ioc_);
        acceptor_->open(endpoint.protocol());
        acceptor_->set_option(asio::ip::tcp::acceptor::reuse_address(true));
        acceptor_->bind(endpoint);
        acceptor_->listen();

        asio::co_spawn(ioc_, accept_loop(), asio::detached);
        spdlog::info("metrics: HTTP /metrics listening on {}", metrics_bind_);
    } catch (const std::exception& e) {
        spdlog::error("metrics: failed to start HTTP listener on {}: {}",
                       metrics_bind_, e.what());
        acceptor_.reset();
    }
}

void MetricsCollector::stop() {
    if (acceptor_) {
        asio::error_code ec;
        acceptor_->close(ec);
        acceptor_.reset();
        spdlog::info("metrics: HTTP /metrics listener stopped");
    }
}

void MetricsCollector::set_metrics_bind(const std::string& bind) {
    if (bind != metrics_bind_) {
        stop();
        metrics_bind_ = bind;
        start();
        spdlog::info("config reload: metrics_bind={}",
                     metrics_bind_.empty() ? "(disabled)" : metrics_bind_);
    }
}

// =============================================================================
// Uptime
// =============================================================================

uint64_t MetricsCollector::uptime_seconds() const {
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count());
}

// =============================================================================
// Accept loop + HTTP handler
// =============================================================================

asio::awaitable<void> MetricsCollector::accept_loop() {
    while (!stopping_.load(std::memory_order_relaxed) && acceptor_ && acceptor_->is_open()) {
        auto [ec, socket] = co_await acceptor_->async_accept(
            asio::as_tuple(asio::use_awaitable));
        if (ec || stopping_.load(std::memory_order_relaxed)) co_return;
        asio::co_spawn(ioc_, handle_connection(std::move(socket)), asio::detached);
    }
}

asio::awaitable<void> MetricsCollector::handle_connection(asio::ip::tcp::socket socket) {
    // Read HTTP request headers (until \r\n\r\n or buffer full)
    std::string request;
    request.reserve(4096);
    std::array<char, 1024> buf{};

    // 5-second timeout via timer expiry check
    asio::steady_timer timeout(ioc_);
    timeout.expires_after(std::chrono::seconds(5));

    for (;;) {
        auto [read_ec, bytes_read] = co_await socket.async_read_some(
            asio::buffer(buf), asio::as_tuple(asio::use_awaitable));
        if (read_ec) co_return;

        request.append(buf.data(), bytes_read);
        if (request.size() > 4096) co_return;  // Buffer overflow, close

        if (request.find("\r\n\r\n") != std::string::npos) break;

        if (timeout.expiry() <= std::chrono::steady_clock::now()) co_return;
    }

    // Check first line for GET /metrics
    auto first_line_end = request.find("\r\n");
    auto first_line = (first_line_end != std::string::npos)
        ? request.substr(0, first_line_end)
        : request;

    std::string response;
    if (first_line.find("GET /metrics") != std::string::npos) {
        size_t active_conns = 0, active_subs = 0;
        if (gauge_provider_) {
            auto [conns, subs] = gauge_provider_();
            active_conns = conns;
            active_subs = subs;
        }
        auto body = format_prometheus(active_conns, active_subs);
        response = "HTTP/1.1 200 OK\r\n"
                   "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
                   "Content-Length: " + std::to_string(body.size()) + "\r\n"
                   "Connection: close\r\n"
                   "\r\n" + body;
    } else {
        response = "HTTP/1.1 404 Not Found\r\n"
                   "Content-Length: 0\r\n"
                   "Connection: close\r\n"
                   "\r\n";
    }

    auto [write_ec, _] = co_await asio::async_write(
        socket, asio::buffer(response),
        asio::as_tuple(asio::use_awaitable));

    asio::error_code ec;
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket.close(ec);
}

// =============================================================================
// Prometheus text exposition format
// =============================================================================

std::string MetricsCollector::format_prometheus(size_t active_connections,
                                                 size_t active_subscriptions) {
    std::string out;
    out.reserve(2048);

    // Counters (7 -- all RelayMetrics fields, per D-04)
    out += "# HELP chromatindb_relay_ws_connections_total Total WebSocket connections since startup.\n"
           "# TYPE chromatindb_relay_ws_connections_total counter\n"
           "chromatindb_relay_ws_connections_total " +
           std::to_string(metrics_.ws_connections_total.load(std::memory_order_relaxed)) + "\n\n";

    out += "# HELP chromatindb_relay_ws_disconnections_total Total WebSocket disconnections since startup.\n"
           "# TYPE chromatindb_relay_ws_disconnections_total counter\n"
           "chromatindb_relay_ws_disconnections_total " +
           std::to_string(metrics_.ws_disconnections_total.load(std::memory_order_relaxed)) + "\n\n";

    out += "# HELP chromatindb_relay_messages_received_total Total messages received since startup.\n"
           "# TYPE chromatindb_relay_messages_received_total counter\n"
           "chromatindb_relay_messages_received_total " +
           std::to_string(metrics_.messages_received_total.load(std::memory_order_relaxed)) + "\n\n";

    out += "# HELP chromatindb_relay_messages_sent_total Total messages sent since startup.\n"
           "# TYPE chromatindb_relay_messages_sent_total counter\n"
           "chromatindb_relay_messages_sent_total " +
           std::to_string(metrics_.messages_sent_total.load(std::memory_order_relaxed)) + "\n\n";

    out += "# HELP chromatindb_relay_auth_failures_total Total authentication failures since startup.\n"
           "# TYPE chromatindb_relay_auth_failures_total counter\n"
           "chromatindb_relay_auth_failures_total " +
           std::to_string(metrics_.auth_failures_total.load(std::memory_order_relaxed)) + "\n\n";

    out += "# HELP chromatindb_relay_rate_limited_total Total rate-limited messages since startup.\n"
           "# TYPE chromatindb_relay_rate_limited_total counter\n"
           "chromatindb_relay_rate_limited_total " +
           std::to_string(metrics_.rate_limited_total.load(std::memory_order_relaxed)) + "\n\n";

    out += "# HELP chromatindb_relay_errors_total Total errors since startup.\n"
           "# TYPE chromatindb_relay_errors_total counter\n"
           "chromatindb_relay_errors_total " +
           std::to_string(metrics_.errors_total.load(std::memory_order_relaxed)) + "\n\n";

    out += "# HELP chromatindb_relay_request_timeouts_total Total request timeouts since startup.\n"
           "# TYPE chromatindb_relay_request_timeouts_total counter\n"
           "chromatindb_relay_request_timeouts_total " +
           std::to_string(metrics_.request_timeouts_total.load(std::memory_order_relaxed)) + "\n\n";

    // Gauges (3 -- per D-05)
    out += "# HELP chromatindb_relay_ws_connections_active Current active WebSocket connections.\n"
           "# TYPE chromatindb_relay_ws_connections_active gauge\n"
           "chromatindb_relay_ws_connections_active " +
           std::to_string(active_connections) + "\n\n";

    out += "# HELP chromatindb_relay_subscriptions_active Current active namespace subscriptions.\n"
           "# TYPE chromatindb_relay_subscriptions_active gauge\n"
           "chromatindb_relay_subscriptions_active " +
           std::to_string(active_subscriptions) + "\n\n";

    out += "# HELP chromatindb_relay_uptime_seconds Relay uptime in seconds.\n"
           "# TYPE chromatindb_relay_uptime_seconds gauge\n"
           "chromatindb_relay_uptime_seconds " +
           std::to_string(uptime_seconds()) + "\n";

    return out;
}

} // namespace chromatindb::relay::core
