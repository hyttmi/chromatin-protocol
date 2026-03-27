/// chromatindb_loadgen -- Protocol-compliant load generator for chromatindb.
///
/// Connects to a running node as a real peer (PQ handshake), sends signed blobs
/// at a timer-driven fixed rate, measures ACK latency via pub/sub notifications,
/// and emits JSON statistics to stdout.

#include "db/identity/identity.h"
#include "db/net/connection.h"
#include "db/peer/peer_manager.h"
#include "db/util/hex.h"
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
#include <filesystem>
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
    std::string identity_save_path;   // empty = don't save
    std::string identity_file_path;   // empty = generate fresh
    bool delete_mode = false;
    std::string hashes_from;          // "stdin" = read target hashes from stdin
    bool verbose_blobs = false;       // emit BLOB_FIELDS JSON to stderr per blob
    std::string delegate_pubkey_hex;  // non-empty = delegation mode
    std::string target_namespace_hex; // non-empty = write to this namespace instead of own
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
              << "  --drain-timeout N    Seconds to wait for ACK notifications after last send (default: 5)\n"
              << "  --identity-save DIR  Save identity keypair to directory after generation\n"
              << "  --identity-file DIR  Load identity keypair from directory (node.key + node.pub)\n"
              << "  --delete             Delete mode: send tombstones instead of blobs\n"
              << "  --hashes-from stdin  Read target blob hashes from stdin (one hex hash per line)\n"
              << "  --verbose-blobs      Emit BLOB_FIELDS JSON to stderr for each blob sent\n"
              << "  --delegate PUBKEY_HEX  Create a delegation blob for the given delegate public key\n"
              << "  --namespace NS_HEX     Write to target namespace instead of own (for delegation writes)\n";
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
        } else if (arg == "--identity-save" && i + 1 < argc) {
            cfg.identity_save_path = argv[++i];
        } else if (arg == "--identity-file" && i + 1 < argc) {
            cfg.identity_file_path = argv[++i];
        } else if (arg == "--delete") {
            cfg.delete_mode = true;
        } else if (arg == "--hashes-from" && i + 1 < argc) {
            cfg.hashes_from = argv[++i];
        } else if (arg == "--verbose-blobs") {
            cfg.verbose_blobs = true;
        } else if (arg == "--delegate" && i + 1 < argc) {
            cfg.delegate_pubkey_hex = argv[++i];
        } else if (arg == "--namespace" && i + 1 < argc) {
            cfg.target_namespace_hex = argv[++i];
        }
    }
    if (cfg.delete_mode && !cfg.delegate_pubkey_hex.empty()) {
        std::cerr << "error: --delete and --delegate cannot be combined\n";
        return false;
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
// Hex encoding — uses shared db/util/hex.h
// =============================================================================

using chromatindb::util::to_hex;
using chromatindb::util::from_hex;
using chromatindb::util::from_hex_fixed;

// =============================================================================
// Blob construction
// =============================================================================

chromatindb::wire::BlobData make_signed_blob(
    const chromatindb::identity::NodeIdentity& id,
    const std::vector<uint8_t>& data,
    uint32_t ttl, uint64_t timestamp,
    const std::array<uint8_t, 32>* ns_override = nullptr)
{
    chromatindb::wire::BlobData blob;
    if (ns_override) {
        std::memcpy(blob.namespace_id.data(), ns_override->data(), 32);
    } else {
        std::memcpy(blob.namespace_id.data(), id.namespace_id().data(), 32);
    }
    blob.pubkey.assign(id.public_key().begin(), id.public_key().end());
    blob.data = data;
    blob.ttl = ttl;
    blob.timestamp = timestamp;
    auto signing_input = chromatindb::wire::build_signing_input(
        blob.namespace_id, blob.data, blob.ttl, blob.timestamp);
    blob.signature = id.sign(signing_input);
    return blob;
}

/// Construct a signed tombstone BlobData for the given target hash.
chromatindb::wire::BlobData make_tombstone_request(
    const chromatindb::identity::NodeIdentity& id,
    std::span<const uint8_t, 32> target_hash,
    uint32_t ttl, uint64_t timestamp,
    const std::array<uint8_t, 32>* ns_override = nullptr)
{
    chromatindb::wire::BlobData tombstone;
    if (ns_override) {
        std::memcpy(tombstone.namespace_id.data(), ns_override->data(), 32);
    } else {
        std::memcpy(tombstone.namespace_id.data(), id.namespace_id().data(), 32);
    }
    tombstone.pubkey.assign(id.public_key().begin(), id.public_key().end());
    tombstone.data = chromatindb::wire::make_tombstone_data(target_hash);
    tombstone.ttl = ttl;
    tombstone.timestamp = timestamp;
    auto signing_input = chromatindb::wire::build_signing_input(
        tombstone.namespace_id, tombstone.data, tombstone.ttl, tombstone.timestamp);
    tombstone.signature = id.sign(signing_input);
    return tombstone;
}

/// Construct a signed delegation BlobData granting write access to a delegate.
chromatindb::wire::BlobData make_delegation_blob(
    const chromatindb::identity::NodeIdentity& id,
    std::span<const uint8_t> delegate_pubkey,
    uint32_t ttl, uint64_t timestamp)
{
    chromatindb::wire::BlobData blob;
    std::memcpy(blob.namespace_id.data(), id.namespace_id().data(), 32);
    blob.pubkey.assign(id.public_key().begin(), id.public_key().end());
    blob.data = chromatindb::wire::make_delegation_data(delegate_pubkey);
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

nlohmann::json stats_to_json(const Stats& s, bool mixed, bool delete_mode,
                             const std::vector<std::string>& blob_hashes) {
    nlohmann::json j;
    if (delete_mode) {
        j["scenario"] = "delete";
    } else {
        j["scenario"] = mixed ? "mixed" : "fixed";
    }
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
    j["blob_hashes"] = blob_hashes;
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
        , delete_mode_(cfg.delete_mode)
        , timer_(ioc)
        , drain_timer_(ioc)
    {
    }

    /// Set target hashes for delete mode (called from main before prepare).
    void set_target_hashes(std::vector<std::array<uint8_t, 32>> hashes) {
        target_hashes_ = std::move(hashes);
    }

    /// Set delegate public key for delegation mode (called from main before prepare).
    void set_delegate_pubkey(std::vector<uint8_t> pubkey) {
        delegate_pubkey_ = std::move(pubkey);
    }

    /// Set target namespace override (for delegation writes to foreign namespace).
    void set_target_namespace(std::array<uint8_t, 32> ns) {
        target_namespace_ = ns;
        has_target_namespace_ = true;
    }

    /// Pre-generate random data pools and size class assignments.
    void prepare() {
        if (delete_mode_) {
            // In delete mode, tombstones are always 36 bytes (Small class).
            // No random data pools needed.
            size_classes_.assign(cfg_.count, SizeClass::Small);
            spdlog::info("prepared {} tombstone assignments (delete mode)", cfg_.count);
            return;
        }

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
                   std::vector<uint8_t> payload,
                   uint32_t /*request_id*/) {
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

    /// Get the collected blob hashes (hex strings).
    const std::vector<std::string>& blob_hashes() const { return blob_hashes_; }

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
    /// Subscribe to target namespace (write mode), then start the timed send loop.
    asio::awaitable<void> subscribe_and_send(chromatindb::net::Connection::Ptr conn_ptr) {
        if (!delete_mode_) {
            // Write mode: subscribe to target namespace for notification ACKs
            std::array<uint8_t, 32> ns_array{};
            if (has_target_namespace_) {
                ns_array = target_namespace_;
            } else {
                std::memcpy(ns_array.data(), identity_.namespace_id().data(), 32);
            }
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
        } else {
            spdlog::info("delete mode: skipping subscription (using DeleteAck for latency)");
        }

        // Pre-compute scheduled send times
        auto interval = std::chrono::nanoseconds(
            static_cast<int64_t>(1'000'000'000.0 / static_cast<double>(cfg_.rate)));
        auto start_time = Clock::now();
        std::vector<Clock::time_point> schedule(cfg_.count);
        for (uint64_t i = 0; i < cfg_.count; ++i) {
            schedule[i] = start_time + interval * static_cast<int64_t>(i);
        }

        auto mode_label = delete_mode_ ? "tombstones" : (!delegate_pubkey_.empty() ? "delegation blobs" : "blobs");
        spdlog::info("starting send: {} {} at {} /sec", cfg_.count, mode_label, cfg_.rate);
        first_send_time_ = start_time;

        // Timer-driven send loop
        for (uint64_t i = 0; i < cfg_.count; ++i) {
            // Check if we should stop (storage full or connection closed)
            if (storage_full_.load(std::memory_order_acquire)) {
                spdlog::warn("stopping: node reported StorageFull after {} sends", i);
                break;
            }
            if (connection_closed_.load(std::memory_order_acquire)) {
                spdlog::warn("stopping: connection closed after {} sends", i);
                break;
            }

            // Wait until scheduled time
            timer_.expires_at(schedule[i]);
            auto [ec_wait] = co_await timer_.async_wait(use_nothrow);
            if (ec_wait && ec_wait != asio::error::operation_aborted) {
                spdlog::error("timer error: {}", ec_wait.message());
                break;
            }

            // Use current second timestamp
            auto now_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());

            std::vector<uint8_t> encoded;
            std::array<uint8_t, 32> hash{};
            size_t data_size = 0;

            const auto* ns_ptr = has_target_namespace_ ? &target_namespace_ : nullptr;

            if (delete_mode_) {
                // Delete path: construct tombstone for target hash
                auto tombstone = make_tombstone_request(
                    identity_, target_hashes_[i], cfg_.ttl, now_us, ns_ptr);
                encoded = chromatindb::wire::encode_blob(tombstone);
                hash = chromatindb::wire::blob_hash(
                    std::span<const uint8_t>(encoded));
                data_size = chromatindb::wire::TOMBSTONE_DATA_SIZE;
            } else if (!delegate_pubkey_.empty()) {
                // Delegation path: construct delegation blob
                auto blob = make_delegation_blob(
                    identity_, std::span<const uint8_t>(delegate_pubkey_),
                    cfg_.ttl, now_us);
                encoded = chromatindb::wire::encode_blob(blob);
                hash = chromatindb::wire::blob_hash(
                    std::span<const uint8_t>(encoded));
                data_size = chromatindb::wire::DELEGATION_DATA_SIZE;
            } else {
                // Write path: construct random blob
                data_size = get_data_size(size_classes_[i]);
                const auto& data_buf = get_data_buffer(size_classes_[i]);
                auto blob = make_signed_blob(identity_, data_buf, cfg_.ttl, now_us, ns_ptr);
                encoded = chromatindb::wire::encode_blob(blob);
                hash = chromatindb::wire::blob_hash(
                    std::span<const uint8_t>(encoded));

                // Emit per-blob field details for integration test verification
                if (cfg_.verbose_blobs) {
                    auto signing_digest = chromatindb::wire::build_signing_input(
                        blob.namespace_id, blob.data, blob.ttl, blob.timestamp);
                    nlohmann::json bf;
                    bf["index"] = i;
                    bf["namespace_id"] = to_hex(blob.namespace_id);
                    if (data_buf.size() <= 4096) {
                        bf["data_hex"] = to_hex(std::span<const uint8_t>(data_buf));
                    }
                    bf["data_sha3"] = to_hex(signing_digest);
                    bf["ttl"] = blob.ttl;
                    bf["timestamp"] = blob.timestamp;
                    bf["pubkey_hex"] = to_hex(std::span<const uint8_t>(blob.pubkey));
                    bf["signature_hex"] = to_hex(std::span<const uint8_t>(blob.signature));
                    bf["blob_hash"] = to_hex(hash);
                    bf["signing_digest"] = to_hex(signing_digest);
                    std::cerr << "BLOB_FIELDS:" << bf.dump() << std::endl;
                }
            }

            // Record scheduled time for this blob (for latency measurement)
            auto hash_hex = to_hex(hash);
            pending_sends_[hash_hex] = schedule[i];

            auto msg_type = delete_mode_
                ? chromatindb::wire::TransportMsgType_Delete
                : chromatindb::wire::TransportMsgType_Data;

            auto send_ok = co_await conn_ptr->send_message(
                msg_type, std::span<const uint8_t>(encoded));
            if (!send_ok) {
                ++send_errors_;
                spdlog::warn("send failed for {} {}", mode_label, i);
                continue;
            }

            blob_hashes_.push_back(hash_hex);
            ++blobs_sent_;
            total_bytes_sent_ += data_size;
            last_send_time_ = Clock::now();

            // Progress logging every 10% or every 100
            if (cfg_.count >= 10 && (i + 1) % std::max(cfg_.count / 10, uint64_t{1}) == 0) {
                spdlog::info("progress: {}/{} {} sent", i + 1, cfg_.count, mode_label);
            }
        }

        spdlog::info("send complete: {} {} sent, {} errors",
                     blobs_sent_, mode_label, send_errors_);

        // Drain phase: wait for remaining ACKs
        auto ack_label = delete_mode_ ? "DeleteAcks" : "notifications";
        spdlog::info("draining {} for {}s...", ack_label, cfg_.drain_timeout);
        drain_timer_.expires_after(std::chrono::seconds(cfg_.drain_timeout));
        auto [ec_drain] = co_await drain_timer_.async_wait(use_nothrow);

        spdlog::info("drain complete: received {}/{} {}",
                     latencies_.size(), blobs_sent_, ack_label);

        // Close connection and stop io_context
        conn_->close();
        ioc_.stop();
    }

    /// Handle incoming messages (Notification/DeleteAck for ACK matching, StorageFull).
    void handle_message(chromatindb::wire::TransportMsgType type,
                        std::vector<uint8_t> payload) {
        if (type == chromatindb::wire::TransportMsgType_StorageFull) {
            spdlog::warn("received StorageFull from node");
            storage_full_.store(true, std::memory_order_release);
            return;
        }

        if (type == chromatindb::wire::TransportMsgType_DeleteAck) {
            handle_delete_ack(std::move(payload));
            return;
        }

        if (type == chromatindb::wire::TransportMsgType_Notification) {
            handle_notification(std::move(payload));
            return;
        }

        // Ignore other message types (Ping/Pong, QuotaExceeded, etc.)
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

    /// Parse DeleteAck payload and compute latency.
    void handle_delete_ack(std::vector<uint8_t> payload) {
        // Format: [blob_hash:32][seq_num_be:8][status:1] = 41 bytes
        if (payload.size() != 41) {
            spdlog::warn("unexpected DeleteAck payload size: {}", payload.size());
            return;
        }

        // Extract blob_hash (first 32 bytes)
        std::array<uint8_t, 32> blob_hash{};
        std::memcpy(blob_hash.data(), payload.data(), 32);
        auto hash_hex = to_hex(blob_hash);

        auto it = pending_sends_.find(hash_hex);
        if (it == pending_sends_.end()) {
            return;
        }

        auto scheduled_time = it->second;
        auto now = Clock::now();
        auto latency_ms = std::chrono::duration<double, std::milli>(
            now - scheduled_time).count();
        latencies_.push_back(latency_ms);
        pending_sends_.erase(it);

        // If all DeleteAcks received, cancel drain timer
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
    bool delete_mode_ = false;
    std::vector<uint8_t> delegate_pubkey_;  // non-empty = delegation mode
    std::array<uint8_t, 32> target_namespace_{};  // namespace override for delegation writes
    bool has_target_namespace_ = false;
    chromatindb::net::Connection::Ptr conn_;

    asio::steady_timer timer_;
    asio::steady_timer drain_timer_;

    // Target hashes for delete mode (set from main before prepare)
    std::vector<std::array<uint8_t, 32>> target_hashes_;

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

    // Collected blob hashes (hex strings) for downstream delete consumption
    std::vector<std::string> blob_hashes_;

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

    // Identity lifecycle: load from file or generate fresh
    auto identity = [&]() {
        if (!cfg.identity_file_path.empty()) {
            spdlog::info("loading identity from {}", cfg.identity_file_path);
            return chromatindb::identity::NodeIdentity::load_from(cfg.identity_file_path);
        }
        return chromatindb::identity::NodeIdentity::generate();
    }();

    // Persist identity if requested
    if (!cfg.identity_save_path.empty()) {
        std::filesystem::create_directories(cfg.identity_save_path);
        identity.save_to(cfg.identity_save_path);
        spdlog::info("identity saved to {}", cfg.identity_save_path);
    }

    spdlog::info("namespace: {}", to_hex(identity.namespace_id()));

    // In delete mode, read target hashes from stdin
    std::vector<std::array<uint8_t, 32>> target_hashes;
    if (cfg.delete_mode && cfg.hashes_from == "stdin") {
        std::string line;
        while (std::getline(std::cin, line)) {
            // Skip empty lines
            if (line.empty()) continue;
            // Trim trailing whitespace/CR
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                                     line.back() == ' ')) {
                line.pop_back();
            }
            if (line.empty()) continue;
            target_hashes.push_back(from_hex_fixed<32>(line));
        }
        cfg.count = target_hashes.size();
        spdlog::info("read {} target hashes from stdin", target_hashes.size());
    }

    // In delegation mode, decode the delegate pubkey hex
    std::vector<uint8_t> delegate_pubkey;
    if (!cfg.delegate_pubkey_hex.empty()) {
        delegate_pubkey = from_hex(cfg.delegate_pubkey_hex);
        if (delegate_pubkey.size() != chromatindb::wire::DELEGATION_PUBKEY_SIZE) {
            spdlog::error("--delegate: expected {} hex chars ({} bytes), got {} hex chars ({} bytes)",
                          chromatindb::wire::DELEGATION_PUBKEY_SIZE * 2,
                          chromatindb::wire::DELEGATION_PUBKEY_SIZE,
                          cfg.delegate_pubkey_hex.size(),
                          delegate_pubkey.size());
            return 1;
        }
        spdlog::info("delegation mode: creating delegation blob for delegate");
    }

    // In namespace override mode, decode the target namespace hex
    std::array<uint8_t, 32> target_namespace{};
    bool has_target_namespace = false;
    if (!cfg.target_namespace_hex.empty()) {
        if (cfg.target_namespace_hex.size() != 64) {
            spdlog::error("--namespace: expected 64 hex chars, got {}",
                          cfg.target_namespace_hex.size());
            return 1;
        }
        target_namespace = from_hex_fixed<32>(cfg.target_namespace_hex);
        has_target_namespace = true;
        spdlog::info("target namespace override: {}", cfg.target_namespace_hex);
    }

    asio::io_context ioc;

    LoadGenerator gen(ioc, cfg, identity);
    if (cfg.delete_mode) {
        gen.set_target_hashes(std::move(target_hashes));
    }
    if (!delegate_pubkey.empty()) {
        gen.set_delegate_pubkey(std::move(delegate_pubkey));
    }
    if (has_target_namespace) {
        gen.set_target_namespace(target_namespace);
    }
    gen.prepare();

    // Spawn the main run coroutine
    asio::co_spawn(ioc, gen.run(), asio::detached);

    // Drive the event loop
    ioc.run();

    // Compute and output stats
    auto stats = gen.compute_stats();
    auto json = stats_to_json(stats, cfg.mixed, cfg.delete_mode, gen.blob_hashes());
    std::cout << json.dump(2) << std::endl;

    spdlog::info("done");
    return 0;
}
