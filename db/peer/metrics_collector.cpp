#include "db/peer/metrics_collector.h"
#include "db/config/config.h"
#include "db/logging/logging.h"
#include "db/storage/storage.h"
#include "db/util/hex.h"

#include <spdlog/spdlog.h>

#include <ctime>
#include <iomanip>
#include <sstream>

namespace chromatindb::peer {

using chromatindb::util::to_hex;

MetricsCollector::MetricsCollector(storage::Storage& storage,
                                   asio::io_context& ioc,
                                   const std::string& metrics_bind,
                                   const bool& stopping,
                                   const config::Config& config)
    : storage_(storage)
    , ioc_(ioc)
    , stopping_(stopping)
    , config_(config)
    , metrics_bind_(metrics_bind) {
}

// =============================================================================
// Periodic metrics
// =============================================================================

asio::awaitable<void> MetricsCollector::metrics_timer_loop() {
    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        metrics_timer_ = &timer;
        timer.expires_after(std::chrono::seconds(60));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        metrics_timer_ = nullptr;
        if (ec || stopping_) co_return;
        log_metrics_line();
    }
}

void MetricsCollector::log_metrics_line() {
    auto storage_bytes = storage_.used_data_bytes();
    auto storage_mib = static_cast<double>(storage_bytes) / (1024.0 * 1024.0);
    auto uptime = compute_uptime_seconds();

    // Sum latest_seq_num across namespaces as blob count proxy
    // (O(N namespaces) not O(N blobs), acceptable for periodic logging)
    uint64_t blob_count = 0;
    auto namespaces = storage_.list_namespaces();
    for (const auto& ns : namespaces) {
        blob_count += ns.latest_seq_num;
    }

    spdlog::info("metrics: peers={} connected_total={} disconnected_total={} "
                 "blobs={} storage={:.1f}MiB "
                 "syncs={} ingests={} rejections={} rate_limited={} "
                 "cursor_hits={} cursor_misses={} full_resyncs={} "
                 "quota_rejections={} sync_rejections={} error_responses={} uptime={}",
                 peers_->size(),
                 metrics_.peers_connected_total,
                 metrics_.peers_disconnected_total,
                 blob_count,
                 storage_mib,
                 metrics_.syncs,
                 metrics_.ingests,
                 metrics_.rejections,
                 metrics_.rate_limited,
                 metrics_.cursor_hits,
                 metrics_.cursor_misses,
                 metrics_.full_resyncs,
                 metrics_.quota_rejections,
                 metrics_.sync_rejections,
                 metrics_.error_responses,
                 uptime);
}

uint64_t MetricsCollector::compute_uptime_seconds() const {
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count());
}

// =============================================================================
// SIGUSR1 metrics dump
// =============================================================================

void MetricsCollector::dump_metrics() {
    spdlog::info("=== METRICS DUMP (SIGUSR1) ===");

    // Global counters (same data as periodic line)
    log_metrics_line();

    // Per-peer breakdown
    spdlog::info("  peers: {}", peers_->size());
    for (const auto& peer : *peers_) {
        auto ns_hex = to_hex(peer->connection->peer_pubkey(), 4);
        spdlog::info("    {} (ns:{}...)", peer->address, ns_hex);
    }

    // Extra info from facade (UDS count, compaction stats, etc.)
    if (dump_extra_) {
        auto extra = dump_extra_();
        if (!extra.empty()) {
            // Output is pre-formatted by the callback
            spdlog::info("{}", extra);
        }
    }

    // Quota metrics
    spdlog::info("  quota_rejections: {}", metrics_.quota_rejections);
    spdlog::info("  sync_rejections: {}", metrics_.sync_rejections);

    // Per-namespace stats via list_namespaces()
    auto namespaces = storage_.list_namespaces();
    spdlog::info("  namespaces: {}", namespaces.size());
    for (const auto& ns : namespaces) {
        auto ns_hex = to_hex(
            std::span<const uint8_t>(ns.namespace_id.data(), ns.namespace_id.size()), 4);
        spdlog::info("    ns:{:>8}... latest_seq={}", ns_hex, ns.latest_seq_num);
    }

    spdlog::info("=== END METRICS DUMP ===");
}

// =============================================================================
// Prometheus /metrics HTTP endpoint
// =============================================================================

void MetricsCollector::start_metrics_listener() {
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
        metrics_acceptor_ = std::make_unique<asio::ip::tcp::acceptor>(ioc_);
        metrics_acceptor_->open(endpoint.protocol());
        metrics_acceptor_->set_option(asio::ip::tcp::acceptor::reuse_address(true));
        metrics_acceptor_->bind(endpoint);
        metrics_acceptor_->listen();

        asio::co_spawn(ioc_, metrics_accept_loop(), asio::detached);
        spdlog::info("metrics: HTTP /metrics listening on {}", metrics_bind_);
    } catch (const std::exception& e) {
        spdlog::error("metrics: failed to start HTTP listener on {}: {}",
                       metrics_bind_, e.what());
        metrics_acceptor_.reset();
    }
}

void MetricsCollector::stop_metrics_listener() {
    if (metrics_acceptor_) {
        asio::error_code ec;
        metrics_acceptor_->close(ec);
        metrics_acceptor_.reset();
        spdlog::info("metrics: HTTP /metrics listener stopped");
    }
}

asio::awaitable<void> MetricsCollector::metrics_accept_loop() {
    while (!stopping_ && metrics_acceptor_ && metrics_acceptor_->is_open()) {
        auto [ec, socket] = co_await metrics_acceptor_->async_accept(
            asio::as_tuple(asio::use_awaitable));
        if (ec || stopping_) co_return;
        asio::co_spawn(ioc_, metrics_handle_connection(std::move(socket)),
                       asio::detached);
    }
}

asio::awaitable<void> MetricsCollector::metrics_handle_connection(asio::ip::tcp::socket socket) {
    // Set a 5-second read timeout
    asio::steady_timer timeout(ioc_);
    timeout.expires_after(std::chrono::seconds(5));

    // Read HTTP request headers (until \r\n\r\n or buffer full)
    std::string request;
    request.reserve(4096);
    std::array<char, 1024> buf{};
    bool headers_complete = false;

    for (;;) {
        // Race: read vs timeout
        auto [read_ec, bytes_read] = co_await socket.async_read_some(
            asio::buffer(buf), asio::as_tuple(asio::use_awaitable));
        if (read_ec) {
            co_return;
        }
        request.append(buf.data(), bytes_read);
        if (request.size() > 4096) {
            co_return;  // Buffer overflow, close
        }
        if (request.find("\r\n\r\n") != std::string::npos) {
            headers_complete = true;
            break;
        }
        // Simple timeout check: if timer already expired, bail
        if (timeout.expiry() <= std::chrono::steady_clock::now()) {
            co_return;
        }
    }

    if (!headers_complete) {
        co_return;
    }

    // Check first line for GET /metrics
    auto first_line_end = request.find("\r\n");
    auto first_line = (first_line_end != std::string::npos)
        ? request.substr(0, first_line_end)
        : request;

    std::string response;
    if (first_line.find("GET /metrics") != std::string::npos) {
        auto body = format_prometheus_metrics();
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
    // Ignore write errors -- we're closing anyway

    asio::error_code ec;
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket.close(ec);
}

std::string MetricsCollector::format_prometheus_metrics() {
    std::string out;
    // 8 KiB accommodates existing 17 counter+gauge blocks plus 24 new
    // chromatindb_config_* gauge blocks (~2 KiB) per T-128-04-03.
    out.reserve(8192);

    // Counters (11 -- all NodeMetrics fields, _total suffix)
    out += "# HELP chromatindb_ingests_total Successful blob ingestions since startup.\n"
           "# TYPE chromatindb_ingests_total counter\n"
           "chromatindb_ingests_total " + std::to_string(metrics_.ingests) + "\n"
           "\n";

    out += "# HELP chromatindb_rejections_total Failed blob ingestions since startup.\n"
           "# TYPE chromatindb_rejections_total counter\n"
           "chromatindb_rejections_total " + std::to_string(metrics_.rejections) + "\n"
           "\n";

    out += "# HELP chromatindb_syncs_total Completed sync rounds since startup.\n"
           "# TYPE chromatindb_syncs_total counter\n"
           "chromatindb_syncs_total " + std::to_string(metrics_.syncs) + "\n"
           "\n";

    out += "# HELP chromatindb_rate_limited_total Rate limit disconnections since startup.\n"
           "# TYPE chromatindb_rate_limited_total counter\n"
           "chromatindb_rate_limited_total " + std::to_string(metrics_.rate_limited) + "\n"
           "\n";

    out += "# HELP chromatindb_peers_connected_total Total peer connections since startup.\n"
           "# TYPE chromatindb_peers_connected_total counter\n"
           "chromatindb_peers_connected_total " + std::to_string(metrics_.peers_connected_total) + "\n"
           "\n";

    out += "# HELP chromatindb_peers_disconnected_total Total peer disconnections since startup.\n"
           "# TYPE chromatindb_peers_disconnected_total counter\n"
           "chromatindb_peers_disconnected_total " + std::to_string(metrics_.peers_disconnected_total) + "\n"
           "\n";

    out += "# HELP chromatindb_cursor_hits_total Namespaces skipped via cursor match since startup.\n"
           "# TYPE chromatindb_cursor_hits_total counter\n"
           "chromatindb_cursor_hits_total " + std::to_string(metrics_.cursor_hits) + "\n"
           "\n";

    out += "# HELP chromatindb_cursor_misses_total Namespaces requiring full hash diff since startup.\n"
           "# TYPE chromatindb_cursor_misses_total counter\n"
           "chromatindb_cursor_misses_total " + std::to_string(metrics_.cursor_misses) + "\n"
           "\n";

    out += "# HELP chromatindb_full_resyncs_total Full resync rounds triggered since startup.\n"
           "# TYPE chromatindb_full_resyncs_total counter\n"
           "chromatindb_full_resyncs_total " + std::to_string(metrics_.full_resyncs) + "\n"
           "\n";

    out += "# HELP chromatindb_quota_rejections_total Namespace quota exceeded rejections since startup.\n"
           "# TYPE chromatindb_quota_rejections_total counter\n"
           "chromatindb_quota_rejections_total " + std::to_string(metrics_.quota_rejections) + "\n"
           "\n";

    out += "# HELP chromatindb_sync_rejections_total Sync rate limit rejections since startup.\n"
           "# TYPE chromatindb_sync_rejections_total counter\n"
           "chromatindb_sync_rejections_total " + std::to_string(metrics_.sync_rejections) + "\n"
           "\n";

    out += "# HELP chromatindb_error_responses_total ErrorResponse messages sent since startup.\n"
           "# TYPE chromatindb_error_responses_total counter\n"
           "chromatindb_error_responses_total " + std::to_string(metrics_.error_responses) + "\n"
           "\n";

    // Gauges (5 -- derived current-state values)
    out += "# HELP chromatindb_peers_connected Current number of connected peers.\n"
           "# TYPE chromatindb_peers_connected gauge\n"
           "chromatindb_peers_connected " + std::to_string(peers_->size()) + "\n"
           "\n";

    // Sum latest_seq_num across namespaces as blob count proxy
    uint64_t blob_count = 0;
    auto namespaces = storage_.list_namespaces();
    for (const auto& ns : namespaces) {
        blob_count += ns.latest_seq_num;
    }

    out += "# HELP chromatindb_blobs_stored Total blobs across all namespaces.\n"
           "# TYPE chromatindb_blobs_stored gauge\n"
           "chromatindb_blobs_stored " + std::to_string(blob_count) + "\n"
           "\n";

    out += "# HELP chromatindb_storage_bytes Current storage usage in bytes.\n"
           "# TYPE chromatindb_storage_bytes gauge\n"
           "chromatindb_storage_bytes " + std::to_string(storage_.used_data_bytes()) + "\n"
           "\n";

    out += "# HELP chromatindb_namespaces Number of active namespaces.\n"
           "# TYPE chromatindb_namespaces gauge\n"
           "chromatindb_namespaces " + std::to_string(namespaces.size()) + "\n"
           "\n";

    out += "# HELP chromatindb_uptime_seconds Node uptime in seconds.\n"
           "# TYPE chromatindb_uptime_seconds gauge\n"
           "chromatindb_uptime_seconds " + std::to_string(compute_uptime_seconds()) + "\n";

    // Config gauges (24 -- one per numeric Config field, alphabetical per D-08,
    // mechanical 1:1 mirror of struct field names per D-06, no unit-suffix
    // normalization per D-09). Values read live from config_ reference so
    // post-SIGHUP scrapes reflect new values (METRICS-02).
    out += "\n# HELP chromatindb_config_blob_max_bytes Configured value of Config::blob_max_bytes.\n"
           "# TYPE chromatindb_config_blob_max_bytes gauge\n"
           "chromatindb_config_blob_max_bytes " + std::to_string(config_.blob_max_bytes) + "\n";

    out += "\n# HELP chromatindb_config_blob_transfer_timeout Configured value of Config::blob_transfer_timeout.\n"
           "# TYPE chromatindb_config_blob_transfer_timeout gauge\n"
           "chromatindb_config_blob_transfer_timeout " + std::to_string(config_.blob_transfer_timeout) + "\n";

    out += "\n# HELP chromatindb_config_compaction_interval_hours Configured value of Config::compaction_interval_hours.\n"
           "# TYPE chromatindb_config_compaction_interval_hours gauge\n"
           "chromatindb_config_compaction_interval_hours " + std::to_string(config_.compaction_interval_hours) + "\n";

    out += "\n# HELP chromatindb_config_cursor_stale_seconds Configured value of Config::cursor_stale_seconds.\n"
           "# TYPE chromatindb_config_cursor_stale_seconds gauge\n"
           "chromatindb_config_cursor_stale_seconds " + std::to_string(config_.cursor_stale_seconds) + "\n";

    out += "\n# HELP chromatindb_config_full_resync_interval Configured value of Config::full_resync_interval.\n"
           "# TYPE chromatindb_config_full_resync_interval gauge\n"
           "chromatindb_config_full_resync_interval " + std::to_string(config_.full_resync_interval) + "\n";

    out += "\n# HELP chromatindb_config_log_max_files Configured value of Config::log_max_files.\n"
           "# TYPE chromatindb_config_log_max_files gauge\n"
           "chromatindb_config_log_max_files " + std::to_string(config_.log_max_files) + "\n";

    out += "\n# HELP chromatindb_config_log_max_size_mb Configured value of Config::log_max_size_mb.\n"
           "# TYPE chromatindb_config_log_max_size_mb gauge\n"
           "chromatindb_config_log_max_size_mb " + std::to_string(config_.log_max_size_mb) + "\n";

    out += "\n# HELP chromatindb_config_max_clients Configured value of Config::max_clients.\n"
           "# TYPE chromatindb_config_max_clients gauge\n"
           "chromatindb_config_max_clients " + std::to_string(config_.max_clients) + "\n";

    out += "\n# HELP chromatindb_config_max_peers Configured value of Config::max_peers.\n"
           "# TYPE chromatindb_config_max_peers gauge\n"
           "chromatindb_config_max_peers " + std::to_string(config_.max_peers) + "\n";

    out += "\n# HELP chromatindb_config_max_storage_bytes Configured value of Config::max_storage_bytes.\n"
           "# TYPE chromatindb_config_max_storage_bytes gauge\n"
           "chromatindb_config_max_storage_bytes " + std::to_string(config_.max_storage_bytes) + "\n";

    out += "\n# HELP chromatindb_config_max_subscriptions_per_connection Configured value of Config::max_subscriptions_per_connection.\n"
           "# TYPE chromatindb_config_max_subscriptions_per_connection gauge\n"
           "chromatindb_config_max_subscriptions_per_connection " + std::to_string(config_.max_subscriptions_per_connection) + "\n";

    out += "\n# HELP chromatindb_config_max_sync_sessions Configured value of Config::max_sync_sessions.\n"
           "# TYPE chromatindb_config_max_sync_sessions gauge\n"
           "chromatindb_config_max_sync_sessions " + std::to_string(config_.max_sync_sessions) + "\n";

    out += "\n# HELP chromatindb_config_max_ttl_seconds Configured value of Config::max_ttl_seconds.\n"
           "# TYPE chromatindb_config_max_ttl_seconds gauge\n"
           "chromatindb_config_max_ttl_seconds " + std::to_string(config_.max_ttl_seconds) + "\n";

    out += "\n# HELP chromatindb_config_namespace_quota_bytes Configured value of Config::namespace_quota_bytes.\n"
           "# TYPE chromatindb_config_namespace_quota_bytes gauge\n"
           "chromatindb_config_namespace_quota_bytes " + std::to_string(config_.namespace_quota_bytes) + "\n";

    out += "\n# HELP chromatindb_config_namespace_quota_count Configured value of Config::namespace_quota_count.\n"
           "# TYPE chromatindb_config_namespace_quota_count gauge\n"
           "chromatindb_config_namespace_quota_count " + std::to_string(config_.namespace_quota_count) + "\n";

    out += "\n# HELP chromatindb_config_pex_interval Configured value of Config::pex_interval.\n"
           "# TYPE chromatindb_config_pex_interval gauge\n"
           "chromatindb_config_pex_interval " + std::to_string(config_.pex_interval) + "\n";

    out += "\n# HELP chromatindb_config_rate_limit_burst Configured value of Config::rate_limit_burst.\n"
           "# TYPE chromatindb_config_rate_limit_burst gauge\n"
           "chromatindb_config_rate_limit_burst " + std::to_string(config_.rate_limit_burst) + "\n";

    out += "\n# HELP chromatindb_config_rate_limit_bytes_per_sec Configured value of Config::rate_limit_bytes_per_sec.\n"
           "# TYPE chromatindb_config_rate_limit_bytes_per_sec gauge\n"
           "chromatindb_config_rate_limit_bytes_per_sec " + std::to_string(config_.rate_limit_bytes_per_sec) + "\n";

    out += "\n# HELP chromatindb_config_safety_net_interval_seconds Configured value of Config::safety_net_interval_seconds.\n"
           "# TYPE chromatindb_config_safety_net_interval_seconds gauge\n"
           "chromatindb_config_safety_net_interval_seconds " + std::to_string(config_.safety_net_interval_seconds) + "\n";

    out += "\n# HELP chromatindb_config_strike_cooldown Configured value of Config::strike_cooldown.\n"
           "# TYPE chromatindb_config_strike_cooldown gauge\n"
           "chromatindb_config_strike_cooldown " + std::to_string(config_.strike_cooldown) + "\n";

    out += "\n# HELP chromatindb_config_strike_threshold Configured value of Config::strike_threshold.\n"
           "# TYPE chromatindb_config_strike_threshold gauge\n"
           "chromatindb_config_strike_threshold " + std::to_string(config_.strike_threshold) + "\n";

    out += "\n# HELP chromatindb_config_sync_cooldown_seconds Configured value of Config::sync_cooldown_seconds.\n"
           "# TYPE chromatindb_config_sync_cooldown_seconds gauge\n"
           "chromatindb_config_sync_cooldown_seconds " + std::to_string(config_.sync_cooldown_seconds) + "\n";

    out += "\n# HELP chromatindb_config_sync_timeout Configured value of Config::sync_timeout.\n"
           "# TYPE chromatindb_config_sync_timeout gauge\n"
           "chromatindb_config_sync_timeout " + std::to_string(config_.sync_timeout) + "\n";

    out += "\n# HELP chromatindb_config_worker_threads Configured value of Config::worker_threads.\n"
           "# TYPE chromatindb_config_worker_threads gauge\n"
           "chromatindb_config_worker_threads " + std::to_string(config_.worker_threads) + "\n";

    // chromatindb_sync_skipped_oversized_total: labeled per-peer counter (METRICS-03 / SYNC-04).
    // Wave 2 increments this via increment_sync_skipped_oversized() on every sync-out skip
    // caused by peer.advertised_blob_cap < blob_size. HELP/TYPE are emitted even when the
    // map is empty (Prometheus convention: HELP/TYPE without samples is legal and makes
    // scrape diffs stable across first-blob and first-skip transitions).
    // sync_skipped_oversized_per_peer_ is a std::map → keys are already alphabetical.
    out += "\n# HELP chromatindb_sync_skipped_oversized_total Blobs skipped on sync-out because peer's advertised cap is smaller.\n"
           "# TYPE chromatindb_sync_skipped_oversized_total counter\n";
    for (const auto& [peer_addr, count] : sync_skipped_oversized_per_peer_) {
        out += "chromatindb_sync_skipped_oversized_total{peer=\"" + peer_addr + "\"} " +
               std::to_string(count) + "\n";
    }

    return out;
}

void MetricsCollector::increment_sync_skipped_oversized(const std::string& peer_address) {
    // Strand-confined to io_context; no mutex needed. std::map::operator[] default-
    // constructs uint64_t to 0 on first insert for this peer address.
    ++sync_skipped_oversized_per_peer_[peer_address];
}

std::string MetricsCollector::prometheus_metrics_text() {
    return format_prometheus_metrics();
}

void MetricsCollector::set_metrics_bind(const std::string& bind) {
    if (bind != metrics_bind_) {
        if (!metrics_bind_.empty()) {
            stop_metrics_listener();
        }
        metrics_bind_ = bind;
        if (!metrics_bind_.empty()) {
            start_metrics_listener();
        }
        spdlog::info("config reload: metrics_bind={}",
                     metrics_bind_.empty() ? "(disabled)" : metrics_bind_);
    }
}

void MetricsCollector::cancel_timers() {
    if (metrics_timer_) metrics_timer_->cancel();
    stop_metrics_listener();
}

} // namespace chromatindb::peer
