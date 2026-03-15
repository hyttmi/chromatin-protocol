/// chromatindb_loadgen -- Protocol-compliant load generator for chromatindb.
///
/// Connects to a running node as a real peer (PQ handshake), sends signed blobs
/// at a timer-driven fixed rate, measures ACK latency via pub/sub notifications,
/// and emits JSON statistics to stdout.

#include "db/identity/identity.h"
#include "db/net/connection.h"
#include "db/peer/peer_manager.h"
#include "db/wire/codec.h"
#include "db/wire/transport_generated.h"

#include <asio.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using chromatindb::net::use_nothrow;

// =============================================================================
// CLI configuration
// =============================================================================

struct Config {
    std::string host;
    std::string port;
    uint64_t count = 100;
    uint64_t rate = 10;
    size_t size = 1024;
    bool mixed = false;
    uint32_t ttl = 3600;
    uint64_t drain_timeout = 5;
};

void print_usage(const char* prog) {
    std::cerr << "chromatindb_loadgen -- load generator for chromatindb\n\n"
              << "Usage: " << prog << " --target HOST:PORT [options]\n\n"
              << "Options:\n"
              << "  --target HOST:PORT   Target node address (required)\n"
              << "  --count N            Total blobs to send (default: 100)\n"
              << "  --rate N             Blobs per second (default: 10)\n"
              << "  --size N             Blob data size in bytes (default: 1024, ignored in mixed mode)\n"
              << "  --mixed              Mixed-size mode: 70% 1K, 20% 100K, 10% 1M\n"
              << "  --ttl N              Blob TTL in seconds (default: 3600)\n"
              << "  --drain-timeout N    Seconds to wait for ACK notifications after last send (default: 5)\n";
}

bool parse_args(int argc, char* argv[], Config& cfg) {
    bool has_target = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return false;
        }
        if (arg == "--target" && i + 1 < argc) {
            std::string target = argv[++i];
            auto colon = target.rfind(':');
            if (colon == std::string::npos || colon == 0 || colon == target.size() - 1) {
                std::cerr << "error: --target must be HOST:PORT\n";
                return false;
            }
            cfg.host = target.substr(0, colon);
            cfg.port = target.substr(colon + 1);
            has_target = true;
        } else if (arg == "--count" && i + 1 < argc) {
            cfg.count = std::stoull(argv[++i]);
        } else if (arg == "--rate" && i + 1 < argc) {
            cfg.rate = std::stoull(argv[++i]);
        } else if (arg == "--size" && i + 1 < argc) {
            cfg.size = std::stoull(argv[++i]);
        } else if (arg == "--mixed") {
            cfg.mixed = true;
        } else if (arg == "--ttl" && i + 1 < argc) {
            cfg.ttl = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--drain-timeout" && i + 1 < argc) {
            cfg.drain_timeout = std::stoull(argv[++i]);
        }
    }
    if (!has_target) {
        std::cerr << "error: --target is required\n\n";
        print_usage(argv[0]);
        return false;
    }
    return true;
}

// =============================================================================
// Size class selection
// =============================================================================

enum class SizeClass : uint8_t { Small, Medium, Large };

constexpr size_t SIZE_SMALL  = 1024;         // 1 KiB
constexpr size_t SIZE_MEDIUM = 100 * 1024;   // 100 KiB
constexpr size_t SIZE_LARGE  = 1024 * 1024;  // 1 MiB

size_t size_class_bytes(SizeClass sc) {
    switch (sc) {
        case SizeClass::Small:  return SIZE_SMALL;
        case SizeClass::Medium: return SIZE_MEDIUM;
        case SizeClass::Large:  return SIZE_LARGE;
    }
    return SIZE_SMALL;
}

/// Pre-compute size class assignments for all blobs.
std::vector<SizeClass> compute_size_classes(uint64_t count, bool mixed,
                                             size_t fixed_size, uint64_t seed) {
    std::vector<SizeClass> classes(count);
    if (!mixed) {
        // Map fixed_size to nearest size class for accounting.
        // In fixed mode, actual data size = fixed_size, but we assign a class
        // for the stats breakdown.
        SizeClass sc = SizeClass::Small;
        if (fixed_size >= SIZE_LARGE) sc = SizeClass::Large;
        else if (fixed_size >= SIZE_MEDIUM) sc = SizeClass::Medium;
        std::fill(classes.begin(), classes.end(), sc);
        return classes;
    }
    // Mixed mode: 70% small, 20% medium, 10% large
    std::mt19937 rng(seed);
    std::discrete_distribution<int> dist({70, 20, 10});
    for (uint64_t i = 0; i < count; ++i) {
        classes[i] = static_cast<SizeClass>(dist(rng));
    }
    return classes;
}

// =============================================================================
// Hex encoding for blob hash keys
// =============================================================================

std::string to_hex(const std::array<uint8_t, 32>& bytes) {
    static constexpr char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (auto b : bytes) {
        result += hex_chars[(b >> 4) & 0xF];
        result += hex_chars[b & 0xF];
    }
    return result;
}

std::string to_hex(std::span<const uint8_t> bytes) {
    static constexpr char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (auto b : bytes) {
        result += hex_chars[(b >> 4) & 0xF];
        result += hex_chars[b & 0xF];
    }
    return result;
}

// =============================================================================
// Blob construction
// =============================================================================

chromatindb::wire::BlobData make_signed_blob(
    const chromatindb::identity::NodeIdentity& id,
    const std::vector<uint8_t>& data,
    uint32_t ttl, uint64_t timestamp)
{
    chromatindb::wire::BlobData blob;
    std::memcpy(blob.namespace_id.data(), id.namespace_id().data(), 32);
    blob.pubkey.assign(id.public_key().begin(), id.public_key().end());
    blob.data = data;
    blob.ttl = ttl;
    blob.timestamp = timestamp;
    auto signing_input = chromatindb::wire::build_signing_input(
        blob.namespace_id, blob.data, blob.ttl, blob.timestamp);
    blob.signature = id.sign(signing_input);
    return blob;
}

// =============================================================================
// Statistics computation
// =============================================================================

struct Stats {
    uint64_t total_blobs = 0;
    double duration_sec = 0.0;
    double blobs_per_sec = 0.0;
    double mib_per_sec = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double min_ms = 0.0;
    double max_ms = 0.0;
    double mean_ms = 0.0;
    uint64_t small_count = 0;
    uint64_t medium_count = 0;
    uint64_t large_count = 0;
    uint64_t errors = 0;
    uint64_t notifications_received = 0;
    uint64_t notifications_expected = 0;
    uint64_t total_bytes_sent = 0;
};

double percentile(std::vector<double>& sorted_latencies, double pct) {
    if (sorted_latencies.empty()) return 0.0;
    auto idx = static_cast<size_t>(pct * static_cast<double>(sorted_latencies.size()));
    if (idx >= sorted_latencies.size()) idx = sorted_latencies.size() - 1;
    return sorted_latencies[idx];
}

nlohmann::json stats_to_json(const Stats& s, bool mixed) {
    nlohmann::json j;
    j["scenario"] = mixed ? "mixed" : "fixed";
    j["total_blobs"] = s.total_blobs;
    j["duration_sec"] = s.duration_sec;
    j["blobs_per_sec"] = s.blobs_per_sec;
    j["mib_per_sec"] = s.mib_per_sec;
    j["latency_ms"] = {
        {"p50", s.p50},
        {"p95", s.p95},
        {"p99", s.p99},
        {"min", s.min_ms},
        {"max", s.max_ms},
        {"mean", s.mean_ms}
    };
    j["blob_sizes"] = {
        {"small_1k", s.small_count},
        {"medium_100k", s.medium_count},
        {"large_1m", s.large_count}
    };
    j["errors"] = s.errors;
    j["notifications_received"] = s.notifications_received;
    j["notifications_expected"] = s.notifications_expected;
    return j;
}

// =============================================================================
// Load generator core
// =============================================================================

class LoadGenerator {
public:
    LoadGenerator(asio::io_context& ioc, const Config& cfg,
                  chromatindb::identity::NodeIdentity& identity)
        : ioc_(ioc)
        , cfg_(cfg)
        , identity_(identity)
        , timer_(ioc)
        , drain_timer_(ioc)
    {
    }

    /// Pre-generate random data pools and size class assignments.
    void prepare() {
        // Pre-generate one random data buffer per size class
        std::mt19937 rng(42);
        auto fill_random = [&](std::vector<uint8_t>& buf, size_t sz) {
            buf.resize(sz);
            std::uniform_int_distribution<int> dist(0, 255);
            for (size_t i = 0; i < sz; ++i) {
                buf[i] = static_cast<uint8_t>(dist(rng));
            }
        };

        fill_random(pool_small_, SIZE_SMALL);
        fill_random(pool_medium_, SIZE_MEDIUM);
        fill_random(pool_large_, SIZE_LARGE);

        // In fixed (non-mixed) mode where size doesn't match a class, create a custom pool
        if (!cfg_.mixed && cfg_.size != SIZE_SMALL &&
            cfg_.size != SIZE_MEDIUM && cfg_.size != SIZE_LARGE) {
            fill_random(pool_custom_, cfg_.size);
        }

        // Pre-compute size class assignments
        size_classes_ = compute_size_classes(cfg_.count, cfg_.mixed, cfg_.size, 12345);

        spdlog::info("prepared {} blob size assignments", cfg_.count);
    }

    /// Connect to the target node and start the load generation coroutine.
    asio::awaitable<void> run() {
        // Resolve target address
        asio::ip::tcp::resolver resolver(ioc_);
        auto [ec_resolve, endpoints] = co_await resolver.async_resolve(
            cfg_.host, cfg_.port, use_nothrow);
        if (ec_resolve) {
            spdlog::error("failed to resolve {}:{}: {}", cfg_.host, cfg_.port,
                          ec_resolve.message());
            co_return;
        }

        // Connect TCP
        asio::ip::tcp::socket socket(ioc_);
        auto [ec_connect, endpoint] = co_await asio::async_connect(
            socket, endpoints, use_nothrow);
        if (ec_connect) {
            spdlog::error("failed to connect to {}:{}: {}", cfg_.host, cfg_.port,
                          ec_connect.message());
            co_return;
        }
        spdlog::info("connected to {}:{}", cfg_.host, cfg_.port);

        // Create outbound connection (PQ handshake handled by Connection)
        conn_ = chromatindb::net::Connection::create_outbound(
            std::move(socket), identity_);

        // Set up message callback for Notification matching
        conn_->on_message(
            [this](chromatindb::net::Connection::Ptr /*conn*/,
                   chromatindb::wire::TransportMsgType type,
                   std::vector<uint8_t> payload) {
                handle_message(type, std::move(payload));
            });

        // Set up close callback
        conn_->on_close(
            [this](chromatindb::net::Connection::Ptr /*conn*/, bool graceful) {
                spdlog::warn("connection closed (graceful={})", graceful);
                connection_closed_.store(true, std::memory_order_release);
                // Cancel drain timer to unblock stats generation
                drain_timer_.cancel();
            });

        // Set up ready callback: subscribe + start send coroutine
        conn_->on_ready(
            [this](chromatindb::net::Connection::Ptr conn_ptr) {
                spdlog::info("handshake complete, subscribing and starting load");
                asio::co_spawn(ioc_, subscribe_and_send(conn_ptr), asio::detached);
            });

        // Run connection lifecycle (handshake -> message loop)
        auto ok = co_await conn_->run();
        if (!ok) {
            spdlog::error("connection handshake or message loop failed");
        }
    }

    /// Get the computed statistics after the run completes.
    Stats compute_stats() {
        Stats s;
        s.total_blobs = blobs_sent_;
        s.total_bytes_sent = total_bytes_sent_;
        s.notifications_received = latencies_.size();
        s.notifications_expected = blobs_sent_;
        s.errors = send_errors_ + (blobs_sent_ - latencies_.size());

        // Duration from first send to last send
        if (blobs_sent_ > 0) {
            auto dur = std::chrono::duration<double>(last_send_time_ - first_send_time_);
            s.duration_sec = dur.count();
            if (s.duration_sec > 0) {
                s.blobs_per_sec = static_cast<double>(blobs_sent_) / s.duration_sec;
                s.mib_per_sec = static_cast<double>(total_bytes_sent_) /
                                (1024.0 * 1024.0) / s.duration_sec;
            }
        }

        // Size class counts
        for (uint64_t i = 0; i < blobs_sent_ && i < size_classes_.size(); ++i) {
            switch (size_classes_[i]) {
                case SizeClass::Small:  ++s.small_count; break;
                case SizeClass::Medium: ++s.medium_count; break;
                case SizeClass::Large:  ++s.large_count; break;
            }
        }

        // Percentile computation
        if (!latencies_.empty()) {
            std::sort(latencies_.begin(), latencies_.end());
            s.p50 = percentile(latencies_, 0.50);
            s.p95 = percentile(latencies_, 0.95);
            s.p99 = percentile(latencies_, 0.99);
            s.min_ms = latencies_.front();
            s.max_ms = latencies_.back();
            double sum = std::accumulate(latencies_.begin(), latencies_.end(), 0.0);
            s.mean_ms = sum / static_cast<double>(latencies_.size());
        }

        return s;
    }

private:
    /// Subscribe to own namespace, then start the timed send loop.
    asio::awaitable<void> subscribe_and_send(chromatindb::net::Connection::Ptr conn_ptr) {
        // Subscribe to our namespace for notification ACKs
        std::array<uint8_t, 32> ns_array{};
        std::memcpy(ns_array.data(), identity_.namespace_id().data(), 32);
        std::vector<std::array<uint8_t, 32>> namespaces = {ns_array};
        auto ns_payload = chromatindb::peer::PeerManager::encode_namespace_list(namespaces);

        auto ok = co_await conn_ptr->send_message(
            chromatindb::wire::TransportMsgType_Subscribe,
            std::span<const uint8_t>(ns_payload));
        if (!ok) {
            spdlog::error("failed to send Subscribe message");
            co_return;
        }
        spdlog::info("subscribed to own namespace for notification ACKs");

        // Pre-compute scheduled send times
        auto interval = std::chrono::nanoseconds(
            static_cast<int64_t>(1'000'000'000.0 / static_cast<double>(cfg_.rate)));
        auto start_time = Clock::now();
        std::vector<Clock::time_point> schedule(cfg_.count);
        for (uint64_t i = 0; i < cfg_.count; ++i) {
            schedule[i] = start_time + interval * static_cast<int64_t>(i);
        }

        spdlog::info("starting send: {} blobs at {} blobs/sec", cfg_.count, cfg_.rate);
        first_send_time_ = start_time;

        // Timer-driven send loop
        for (uint64_t i = 0; i < cfg_.count; ++i) {
            // Check if we should stop (storage full or connection closed)
            if (storage_full_.load(std::memory_order_acquire)) {
                spdlog::warn("stopping: node reported StorageFull after {} blobs", i);
                break;
            }
            if (connection_closed_.load(std::memory_order_acquire)) {
                spdlog::warn("stopping: connection closed after {} blobs", i);
                break;
            }

            // Wait until scheduled time
            timer_.expires_at(schedule[i]);
            auto [ec_wait] = co_await timer_.async_wait(use_nothrow);
            if (ec_wait && ec_wait != asio::error::operation_aborted) {
                spdlog::error("timer error: {}", ec_wait.message());
                break;
            }

            // Construct and send blob
            auto data_size = get_data_size(size_classes_[i]);
            const auto& data_buf = get_data_buffer(size_classes_[i]);

            // Use current microsecond timestamp for uniqueness
            auto now_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());

            auto blob = make_signed_blob(identity_, data_buf, cfg_.ttl, now_us);
            auto encoded = chromatindb::wire::encode_blob(blob);
            auto hash = chromatindb::wire::blob_hash(
                std::span<const uint8_t>(encoded));

            // Record scheduled time for this blob (for latency measurement)
            auto hash_hex = to_hex(hash);
            pending_sends_[hash_hex] = schedule[i];

            auto send_ok = co_await conn_ptr->send_message(
                chromatindb::wire::TransportMsgType_Data,
                std::span<const uint8_t>(encoded));
            if (!send_ok) {
                ++send_errors_;
                spdlog::warn("send failed for blob {}", i);
                continue;
            }

            ++blobs_sent_;
            total_bytes_sent_ += data_size;
            last_send_time_ = Clock::now();

            // Progress logging every 10% or every 100 blobs
            if (cfg_.count >= 10 && (i + 1) % std::max(cfg_.count / 10, uint64_t{1}) == 0) {
                spdlog::info("progress: {}/{} blobs sent", i + 1, cfg_.count);
            }
        }

        spdlog::info("send complete: {} blobs sent, {} errors", blobs_sent_, send_errors_);

        // Drain phase: wait for remaining notifications
        spdlog::info("draining notifications for {}s...", cfg_.drain_timeout);
        drain_timer_.expires_after(std::chrono::seconds(cfg_.drain_timeout));
        auto [ec_drain] = co_await drain_timer_.async_wait(use_nothrow);

        spdlog::info("drain complete: received {}/{} notifications",
                     latencies_.size(), blobs_sent_);

        // Close connection and stop io_context
        conn_->close();
        ioc_.stop();
    }

    /// Handle incoming messages (Notification for ACK matching, StorageFull).
    void handle_message(chromatindb::wire::TransportMsgType type,
                        std::vector<uint8_t> payload) {
        if (type == chromatindb::wire::TransportMsgType_StorageFull) {
            spdlog::warn("received StorageFull from node");
            storage_full_.store(true, std::memory_order_release);
            return;
        }

        if (type == chromatindb::wire::TransportMsgType_Notification) {
            handle_notification(std::move(payload));
            return;
        }

        // Ignore other message types (Ping/Pong, etc.)
    }

    /// Parse notification payload and compute latency.
    void handle_notification(std::vector<uint8_t> payload) {
        // Format: [namespace_id:32][blob_hash:32][seq_num:8][blob_size:4][is_tombstone:1]
        if (payload.size() != 77) {
            spdlog::warn("unexpected notification payload size: {}", payload.size());
            return;
        }

        // Extract blob_hash (bytes 32-63)
        std::array<uint8_t, 32> blob_hash{};
        std::memcpy(blob_hash.data(), payload.data() + 32, 32);
        auto hash_hex = to_hex(blob_hash);

        auto it = pending_sends_.find(hash_hex);
        if (it == pending_sends_.end()) {
            // Could be a notification for a blob we didn't send (e.g., from another peer)
            return;
        }

        auto scheduled_time = it->second;
        auto now = Clock::now();
        auto latency_ms = std::chrono::duration<double, std::milli>(
            now - scheduled_time).count();
        latencies_.push_back(latency_ms);
        pending_sends_.erase(it);

        // If all notifications received, cancel drain timer
        if (latencies_.size() == blobs_sent_ && blobs_sent_ > 0) {
            drain_timer_.cancel();
        }
    }

    /// Get the actual data size for a blob based on size class and config.
    size_t get_data_size(SizeClass sc) const {
        if (!cfg_.mixed) return cfg_.size;
        return size_class_bytes(sc);
    }

    /// Get the pre-generated data buffer for the given size class.
    const std::vector<uint8_t>& get_data_buffer(SizeClass sc) const {
        if (!cfg_.mixed) {
            // In fixed mode, use custom buffer if size doesn't match any class
            if (!pool_custom_.empty()) return pool_custom_;
            switch (sc) {
                case SizeClass::Small:  return pool_small_;
                case SizeClass::Medium: return pool_medium_;
                case SizeClass::Large:  return pool_large_;
            }
        }
        switch (sc) {
            case SizeClass::Small:  return pool_small_;
            case SizeClass::Medium: return pool_medium_;
            case SizeClass::Large:  return pool_large_;
        }
        return pool_small_;
    }

    asio::io_context& ioc_;
    const Config& cfg_;
    chromatindb::identity::NodeIdentity& identity_;
    chromatindb::net::Connection::Ptr conn_;

    asio::steady_timer timer_;
    asio::steady_timer drain_timer_;

    // Pre-generated random data pools
    std::vector<uint8_t> pool_small_;
    std::vector<uint8_t> pool_medium_;
    std::vector<uint8_t> pool_large_;
    std::vector<uint8_t> pool_custom_;  // For fixed sizes that don't match a class

    // Size class assignments
    std::vector<SizeClass> size_classes_;

    // Pending sends: blob_hash_hex -> scheduled send time
    std::unordered_map<std::string, Clock::time_point> pending_sends_;

    // Collected latencies (milliseconds)
    std::vector<double> latencies_;

    // Counters
    uint64_t blobs_sent_ = 0;
    uint64_t send_errors_ = 0;
    uint64_t total_bytes_sent_ = 0;
    Clock::time_point first_send_time_;
    Clock::time_point last_send_time_;

    // Flags
    std::atomic<bool> storage_full_{false};
    std::atomic<bool> connection_closed_{false};
};

} // anonymous namespace

int main(int argc, char* argv[]) {
    // Configure spdlog to stderr so stdout is clean JSON
    auto logger = spdlog::stderr_color_mt("loadgen");
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");

    Config cfg;
    if (!parse_args(argc, argv, cfg)) {
        return 1;
    }

    spdlog::info("target: {}:{}", cfg.host, cfg.port);
    spdlog::info("count: {}, rate: {}/s, size: {}, mixed: {}, ttl: {}s",
                 cfg.count, cfg.rate, cfg.size, cfg.mixed, cfg.ttl);

    // Generate ephemeral identity for this load run
    auto identity = chromatindb::identity::NodeIdentity::generate();
    spdlog::info("namespace: {}", to_hex(identity.namespace_id()));

    asio::io_context ioc;

    LoadGenerator gen(ioc, cfg, identity);
    gen.prepare();

    // Spawn the main run coroutine
    asio::co_spawn(ioc, gen.run(), asio::detached);

    // Drive the event loop
    ioc.run();

    // Compute and output stats
    auto stats = gen.compute_stats();
    auto json = stats_to_json(stats, cfg.mixed);
    std::cout << json.dump(2) << std::endl;

    spdlog::info("done");
    return 0;
}
