#pragma once

#include "db/peer/peer_types.h"

#include <asio.hpp>

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>

namespace chromatindb::storage { class Storage; }

namespace chromatindb::peer {

/// Owns NodeMetrics counters, Prometheus /metrics HTTP endpoint, periodic
/// metrics logging, and SIGUSR1 dump output.  Extracted from PeerManager
/// (ARCH-01, component D-06).
class MetricsCollector {
public:
    /// Callback to provide extra dump info from facade (UDS count, compaction).
    using DumpExtraCallback = std::function<std::string()>;

    MetricsCollector(storage::Storage& storage, asio::io_context& ioc,
                     const std::string& metrics_bind, const bool& stopping);

    // Timer loops (co_spawn from PeerManager::start)
    asio::awaitable<void> metrics_timer_loop();
    asio::awaitable<void> metrics_accept_loop();

    // Lifecycle
    void start_metrics_listener();
    void stop_metrics_listener();
    void cancel_timers();

    // Metrics access
    NodeMetrics& node_metrics() { return metrics_; }
    const NodeMetrics& node_metrics() const { return metrics_; }
    uint64_t compute_uptime_seconds() const;
    void dump_metrics();
    void log_metrics_line();
    std::string format_prometheus_metrics();
    std::string prometheus_metrics_text();

    // Config reload
    void set_metrics_bind(const std::string& bind);

    // Set callback for extra dump info (UDS count, compaction stats)
    void set_dump_extra(DumpExtraCallback cb) { dump_extra_ = std::move(cb); }

    // Set peers reference (called after ConnectionManager is constructed)
    void set_peers(const std::deque<std::unique_ptr<PeerInfo>>& peers) { peers_ = &peers; }

    // Record start time (called from PeerManager::start)
    void record_start_time() { start_time_ = std::chrono::steady_clock::now(); }

private:
    asio::awaitable<void> metrics_handle_connection(asio::ip::tcp::socket socket);

    storage::Storage& storage_;
    asio::io_context& ioc_;
    const bool& stopping_;
    const std::deque<std::unique_ptr<PeerInfo>>* peers_ = nullptr;

    NodeMetrics metrics_;
    std::chrono::steady_clock::time_point start_time_;
    asio::steady_timer* metrics_timer_ = nullptr;
    std::unique_ptr<asio::ip::tcp::acceptor> metrics_acceptor_;
    std::string metrics_bind_;
    DumpExtraCallback dump_extra_;
};

} // namespace chromatindb::peer
